/*
 * riffmaster_game.sprx - runs inside the game process (injected by
 * riffmaster_loader.sprx) and makes a PDP Riffmaster (PS4) look like a PS3
 * Guitar Hero / Rock Band guitar.
 *
 *  1. Drives the Riffmaster directly through cellUsbd (the PS3 system ignores it).
 *  2. Translates its 64-byte input report (see riffmaster_report_map.md) into
 *     CellPadData and feeds it to a virtual pad registered with cellPadLdd.
 *  3. Redirects the game's sys_io imports for cellPadGetInfo / GetInfo2 /
 *     PeriphGetInfo / PeriphGetData so that virtual pad reports a guitar's
 *     vendor/product ID and peripheral class.
 */

#include <stdint.h>
#include <stdbool.h>
#include <sys/prx.h>
#include <sys/ppu_thread.h>
#include <sys/timer.h>
#include <sys/process.h>
#include <sys/synchronization.h>
#include <cell/sysmodule.h>
#include <cell/pad.h>
#include <cell/usbd.h>
#include <cell/cell_fs.h>

#include "../common/util.h"

#ifndef USB_CLASS_HID
#define USB_CLASS_HID 0x03
#endif
#include "../common/ps3mapi.h"

SYS_MODULE_INFO(riffmaster_game, 0, 1, 0);
SYS_MODULE_START(riff_start);
SYS_MODULE_STOP(riff_stop);

/* ------------------------------------------------------------------------ */
/* Riffmaster input report (byte 0 = report ID)                              */
/* ------------------------------------------------------------------------ */

#define RM_VID          0x0E6F
/* TODO: narrow to the Riffmaster's exact PID (run `hid_inspect.py list --vid 0x0e6f`). */
#define RM_PID_MIN      0x0000
#define RM_PID_MAX      0xFFFF

#define RM_REPORT_ID    0x01
#define RM_REPORT_SIZE  64
#define RM_OFF_HAT      5   /* low nibble: hat (strum), 0xF = neutral */
#define RM_OFF_BTN6     6   /* 0x01 orange, 0x10 select, 0x20 start */
#define RM_OFF_BTN7     7   /* 0x01 PS */
#define RM_OFF_WHAMMY   44  /* 0x00 rest .. 0xFF full */
#define RM_OFF_TILT     45  /* ~0x1D rest .. ~0xF2 full, noisy */
#define RM_OFF_FRETS    46  /* 0x01 G, 0x02 R, 0x04 Y, 0x08 B, 0x10 O */

#define RM_FRET_GREEN   0x01
#define RM_FRET_RED     0x02
#define RM_FRET_YELLOW  0x04
#define RM_FRET_BLUE    0x08
#define RM_FRET_ORANGE  0x10

#define RM_BTN6_SELECT  0x10
#define RM_BTN6_START   0x20

#define RM_TILT_MIN     0x40  /* readings at or below this count as level (rest drifts to ~0x33) */
#define RM_TILT_MAX     0xE8  /* readings at or above this count as fully tilted */
#define RM_TILT_ON      0x80  /* hysteresis for the digital star-power output */
#define RM_TILT_OFF     0x50

typedef struct {
	uint8_t frets;
	uint8_t hat;
	uint8_t btn6;
	uint8_t whammy;
	uint8_t tilt;
	bool    tilted;
} rm_state_t;

/* ------------------------------------------------------------------------ */
/* Output profiles                                                           */
/* ------------------------------------------------------------------------ */

typedef struct {
	const char *name;
	uint16_t vid, pid;
	/* Right stick X range the game expects for the whammy bar. */
	uint8_t  whammy_rest, whammy_full;
	/* SIXAXIS-style sensor X range for tilt (0x200 = level). */
	uint16_t tilt_rest, tilt_full;
} profile_t;

/* UNVERIFIED: whammy/tilt ranges are best guesses - check them in game. */
static const profile_t PROFILE_GH = { "gh", 0x12BA, 0x0100, 0x7F, 0xFF, 0x200, 0x180 };
static const profile_t PROFILE_RB = { "rb", 0x12BA, 0x0200, 0x7F, 0xFF, 0x200, 0x180 };

static const profile_t *g_profile = &PROFILE_GH;

/* ------------------------------------------------------------------------ */
/* Shared state                                                              */
/* ------------------------------------------------------------------------ */

static sys_lwmutex_t g_lock;
static rm_state_t    g_state;
static int32_t       g_ldd = -1;      /* cellPadLdd handle */
static volatile bool g_running;

static int32_t g_dev_id    = -1;
static int32_t g_ctrl_pipe = -1;
static int32_t g_intr_pipe = -1;
static UsbEndpointDescriptor *g_intr_ep;
static uint8_t *g_buf;  /* from cellUsbdAllocateMemory (DMA-able) */

static sys_ppu_thread_t g_thread;

static int32_t ldd_port(void)
{
	return g_ldd >= 0 ? cellPadLddGetPortNo(g_ldd) : -1;
}

/* ------------------------------------------------------------------------ */
/* Mapping                                                                   */
/* ------------------------------------------------------------------------ */

static uint32_t scale(uint32_t v, uint32_t in_min, uint32_t in_max, int32_t out_a, int32_t out_b)
{
	if (v <= in_min) return (uint32_t)out_a;
	if (v >= in_max) return (uint32_t)out_b;
	return (uint32_t)(out_a + ((int32_t)(v - in_min) * (out_b - out_a)) / (int32_t)(in_max - in_min));
}

static bool strum_up(uint8_t hat)   { return hat == 0 || hat == 1 || hat == 7; }
static bool strum_down(uint8_t hat) { return hat == 3 || hat == 4 || hat == 5; }

static void build_pad_data(const rm_state_t *s, CellPadData *d)
{
	rm_memset(d, 0, sizeof(*d));
	d->len = 24;

	uint16_t d1 = 0, d2 = 0;
	if (strum_up(s->hat))               d1 |= CELL_PAD_CTRL_UP;
	if (strum_down(s->hat))             d1 |= CELL_PAD_CTRL_DOWN;
	if (s->btn6 & RM_BTN6_START)        d1 |= CELL_PAD_CTRL_START;
	/* Select deploys star power / overdrive in GH and RB, so tilt drives it too. */
	if ((s->btn6 & RM_BTN6_SELECT) || s->tilted) d1 |= CELL_PAD_CTRL_SELECT;

	if (s->frets & RM_FRET_GREEN)       d2 |= CELL_PAD_CTRL_CROSS;
	if (s->frets & RM_FRET_RED)         d2 |= CELL_PAD_CTRL_CIRCLE;
	if (s->frets & RM_FRET_YELLOW)      d2 |= CELL_PAD_CTRL_TRIANGLE;
	if (s->frets & RM_FRET_BLUE)        d2 |= CELL_PAD_CTRL_SQUARE;
	if (s->frets & RM_FRET_ORANGE)      d2 |= CELL_PAD_CTRL_L1;

	uint16_t *b = d->button;
	b[CELL_PAD_BTN_OFFSET_DIGITAL1] = d1;
	b[CELL_PAD_BTN_OFFSET_DIGITAL2] = d2;

	b[CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_X] =
		scale(s->whammy, 0x00, 0xFF, g_profile->whammy_rest, g_profile->whammy_full);
	b[CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_Y] = 0x80;
	b[CELL_PAD_BTN_OFFSET_ANALOG_LEFT_X]  = 0x80;
	b[CELL_PAD_BTN_OFFSET_ANALOG_LEFT_Y]  = 0x80;

	b[CELL_PAD_BTN_OFFSET_PRESS_UP]       = (d1 & CELL_PAD_CTRL_UP)       ? 0xFF : 0;
	b[CELL_PAD_BTN_OFFSET_PRESS_DOWN]     = (d1 & CELL_PAD_CTRL_DOWN)     ? 0xFF : 0;
	b[CELL_PAD_BTN_OFFSET_PRESS_CROSS]    = (d2 & CELL_PAD_CTRL_CROSS)    ? 0xFF : 0;
	b[CELL_PAD_BTN_OFFSET_PRESS_CIRCLE]   = (d2 & CELL_PAD_CTRL_CIRCLE)   ? 0xFF : 0;
	b[CELL_PAD_BTN_OFFSET_PRESS_TRIANGLE] = (d2 & CELL_PAD_CTRL_TRIANGLE) ? 0xFF : 0;
	b[CELL_PAD_BTN_OFFSET_PRESS_SQUARE]   = (d2 & CELL_PAD_CTRL_SQUARE)   ? 0xFF : 0;
	b[CELL_PAD_BTN_OFFSET_PRESS_L1]       = (d2 & CELL_PAD_CTRL_L1)       ? 0xFF : 0;

	b[CELL_PAD_BTN_OFFSET_SENSOR_X] =
		scale(s->tilt, RM_TILT_MIN, RM_TILT_MAX, g_profile->tilt_rest, g_profile->tilt_full);
	b[CELL_PAD_BTN_OFFSET_SENSOR_Y] = 0x200;
	b[CELL_PAD_BTN_OFFSET_SENSOR_Z] = 0x200;
	b[CELL_PAD_BTN_OFFSET_SENSOR_G] = 0x200;
}

/* ------------------------------------------------------------------------ */
/* USB driver (cellUsbd extra LDD)                                           */
/* ------------------------------------------------------------------------ */

static void submit_read(void);

static void handle_report(const uint8_t *r, int32_t len)
{
	if (len < RM_OFF_FRETS + 1 || r[0] != RM_REPORT_ID)
		return;

	sys_lwmutex_lock(&g_lock, 0);
	rm_state_t prev = g_state;
	g_state.frets  = r[RM_OFF_FRETS] & 0x1F;
	g_state.hat    = r[RM_OFF_HAT] & 0x0F;
	g_state.btn6   = r[RM_OFF_BTN6];
	g_state.whammy = r[RM_OFF_WHAMMY];
	g_state.tilt   = r[RM_OFF_TILT];
	if (g_state.tilt >= RM_TILT_ON)       g_state.tilted = true;
	else if (g_state.tilt <= RM_TILT_OFF) g_state.tilted = false;

	/* Tilt jitters at rest; only push frames the game would see as different. */
	bool changed = prev.frets != g_state.frets || prev.hat != g_state.hat ||
	               prev.btn6 != g_state.btn6 || prev.whammy != g_state.whammy ||
	               prev.tilted != g_state.tilted ||
	               (g_state.tilt > RM_TILT_MIN && prev.tilt != g_state.tilt);
	rm_state_t snap = g_state;
	sys_lwmutex_unlock(&g_lock);

	if (g_ldd < 0) {
		g_ldd = cellPadLddRegisterController();
		rm_logx("cellPadLddRegisterController", g_ldd);
		if (g_ldd < 0) return;
		rm_logx("virtual pad port", ldd_port());
		changed = true;
	}
	if (changed) {
		CellPadData data;
		build_pad_data(&snap, &data);
		cellPadLddDataInsert(g_ldd, &data);
	}
}

static void read_done(int32_t result, int32_t count, void *arg)
{
	(void)arg;
	if (result == HC_CC_NOERR)
		handle_report(g_buf, count);
	if (g_running && g_intr_pipe >= 0)
		submit_read();
}

static void submit_read(void)
{
	int32_t r = cellUsbdInterruptTransfer(g_intr_pipe, g_buf, RM_REPORT_SIZE, read_done, NULL);
	if (r != CELL_OK)
		rm_logx("cellUsbdInterruptTransfer failed", r);
}

static void set_config_done(int32_t result, int32_t count, void *arg)
{
	(void)count; (void)arg;
	if (result != HC_CC_NOERR) {
		rm_logx("SET_CONFIGURATION failed", result);
		return;
	}
	g_intr_pipe = cellUsbdOpenPipe(g_dev_id, g_intr_ep);
	rm_logx("interrupt pipe", g_intr_pipe);
	if (g_intr_pipe >= 0)
		submit_read();
}

/* First interrupt-IN endpoint of the first HID interface, or NULL. */
static UsbEndpointDescriptor *find_hid_in_endpoint(int32_t dev_id)
{
	UsbInterfaceDescriptor *ifd = NULL;
	while ((ifd = (UsbInterfaceDescriptor *)cellUsbdScanStaticDescriptor(
	            dev_id, ifd, USB_DESCRIPTOR_TYPE_INTERFACE)) != NULL) {
		if (ifd->bInterfaceClass != USB_CLASS_HID)
			continue;
		void *p = ifd;
		UsbEndpointDescriptor *ep;
		while ((ep = (UsbEndpointDescriptor *)cellUsbdScanStaticDescriptor(
		            dev_id, p, USB_DESCRIPTOR_TYPE_ENDPOINT)) != NULL) {
			if ((ep->bEndpointAddress & 0x80) && (ep->bmAttributes & 0x03) == 0x03)
				return ep;
			p = ep;
		}
	}
	return NULL;
}

static int rm_probe(int32_t dev_id)
{
	if (find_hid_in_endpoint(dev_id) == NULL)
		return CELL_USBD_PROBE_FAILED;
	rm_logx("probe ok, dev", dev_id);
	return CELL_USBD_PROBE_SUCCEEDED;
}

static int rm_attach(int32_t dev_id)
{
	UsbConfigurationDescriptor *cfg = (UsbConfigurationDescriptor *)
		cellUsbdScanStaticDescriptor(dev_id, NULL, USB_DESCRIPTOR_TYPE_CONFIGURATION);
	g_intr_ep = find_hid_in_endpoint(dev_id);
	if (cfg == NULL || g_intr_ep == NULL)
		return CELL_USBD_ATTACH_FAILED;

	if (g_buf == NULL && cellUsbdAllocateMemory((void **)&g_buf, RM_REPORT_SIZE) != CELL_OK) {
		rm_log("cellUsbdAllocateMemory failed");
		g_buf = NULL;
		return CELL_USBD_ATTACH_FAILED;
	}

	g_dev_id = dev_id;
	g_ctrl_pipe = cellUsbdOpenPipe(dev_id, NULL);
	if (g_ctrl_pipe < 0) {
		rm_logx("control pipe failed", g_ctrl_pipe);
		return CELL_USBD_ATTACH_FAILED;
	}
	int32_t r = cellUsbdSetConfiguration(g_ctrl_pipe, cfg->bConfigurationValue, set_config_done, NULL);
	rm_logx("attach, SET_CONFIGURATION", r);
	return r == CELL_OK ? CELL_USBD_ATTACH_SUCCEEDED : CELL_USBD_ATTACH_FAILED;
}

static int rm_detach(int32_t dev_id)
{
	rm_logx("detach, dev", dev_id);
	if (g_intr_pipe >= 0) cellUsbdClosePipe(g_intr_pipe);
	if (g_ctrl_pipe >= 0) cellUsbdClosePipe(g_ctrl_pipe);
	g_intr_pipe = g_ctrl_pipe = g_dev_id = -1;
	if (g_ldd >= 0) {
		cellPadLddUnregisterController(g_ldd);
		g_ldd = -1;
	}
	return CELL_USBD_DETACH_SUCCEEDED;
}

static CellUsbdLddOps g_ldd_ops = { "riffmaster", rm_probe, rm_attach, rm_detach };

/* ------------------------------------------------------------------------ */
/* Pad identity hooks                                                        */
/* ------------------------------------------------------------------------ */

#define GUITAR_PROFILE_BITS (                                              \
	CELL_PAD_PCLASS_PROFILE_GUITAR_FRET_1 | CELL_PAD_PCLASS_PROFILE_GUITAR_FRET_2 | \
	CELL_PAD_PCLASS_PROFILE_GUITAR_FRET_3 | CELL_PAD_PCLASS_PROFILE_GUITAR_FRET_4 | \
	CELL_PAD_PCLASS_PROFILE_GUITAR_FRET_5 | CELL_PAD_PCLASS_PROFILE_GUITAR_STRUM_UP | \
	CELL_PAD_PCLASS_PROFILE_GUITAR_STRUM_DOWN | CELL_PAD_PCLASS_PROFILE_GUITAR_WHAMMYBAR | \
	CELL_PAD_PCLASS_PROFILE_GUITAR_TILT_SENS)

#define GUITAR_CAPABILITY (CELL_PAD_CAPABILITY_PS3_CONFORMITY | \
	CELL_PAD_CAPABILITY_PRESS_MODE | CELL_PAD_CAPABILITY_SENSOR_MODE)

static int32_t hook_GetInfo(CellPadInfo *info)
{
	int32_t r = cellPadGetInfo(info);
	int32_t port = ldd_port();
	if (r == CELL_OK && port >= 0 && port < CELL_PAD_MAX_PORT_NUM) {
		info->vendor_id[port]  = g_profile->vid;
		info->product_id[port] = g_profile->pid;
	}
	return r;
}

static int32_t hook_GetInfo2(CellPadInfo2 *info)
{
	int32_t r = cellPadGetInfo2(info);
	int32_t port = ldd_port();
	if (r == CELL_OK && port >= 0 && port < CELL_PAD_MAX_PORT_NUM) {
		info->device_type[port]       = CELL_PAD_DEV_TYPE_STANDARD;
		info->device_capability[port] = GUITAR_CAPABILITY;
	}
	return r;
}

static int32_t hook_PeriphGetInfo(CellPadPeriphInfo *info)
{
	int32_t r = cellPadPeriphGetInfo(info);
	int32_t port = ldd_port();
	if (r == CELL_OK && port >= 0 && port < CELL_PAD_MAX_PORT_NUM) {
		info->device_type[port]       = CELL_PAD_DEV_TYPE_STANDARD;
		info->device_capability[port] = GUITAR_CAPABILITY;
		info->pclass_type[port]       = CELL_PAD_PCLASS_TYPE_GUITAR;
		info->pclass_profile[port]    = GUITAR_PROFILE_BITS;
	}
	return r;
}

static int32_t hook_PeriphGetData(uint32_t port_no, CellPadPeriphData *data)
{
	if ((int32_t)port_no != ldd_port())
		return cellPadPeriphGetData(port_no, data);

	rm_state_t s;
	sys_lwmutex_lock(&g_lock, 0);
	s = g_state;
	sys_lwmutex_unlock(&g_lock);

	CellPadData pad;
	build_pad_data(&s, &pad);
	rm_memset(data, 0, sizeof(*data));
	rm_memcpy(data->cellpad_data.button, pad.button, sizeof(pad.button));
	data->pclass_type       = CELL_PAD_PCLASS_TYPE_GUITAR;
	data->pclass_profile    = GUITAR_PROFILE_BITS;
	data->cellpad_data.len  = CELL_PAD_PCLASS_BTN_OFFSET_GUITAR_TILT_SENS + 1;

	uint16_t *b = data->cellpad_data.button;
	b[CELL_PAD_PCLASS_BTN_OFFSET_GUITAR_FRET_1]     = (s.frets & RM_FRET_GREEN)  ? 0xFF : 0;
	b[CELL_PAD_PCLASS_BTN_OFFSET_GUITAR_FRET_2]     = (s.frets & RM_FRET_RED)    ? 0xFF : 0;
	b[CELL_PAD_PCLASS_BTN_OFFSET_GUITAR_FRET_3]     = (s.frets & RM_FRET_YELLOW) ? 0xFF : 0;
	b[CELL_PAD_PCLASS_BTN_OFFSET_GUITAR_FRET_4]     = (s.frets & RM_FRET_BLUE)   ? 0xFF : 0;
	b[CELL_PAD_PCLASS_BTN_OFFSET_GUITAR_FRET_5]     = (s.frets & RM_FRET_ORANGE) ? 0xFF : 0;
	b[CELL_PAD_PCLASS_BTN_OFFSET_GUITAR_STRUM_UP]   = strum_up(s.hat)   ? 0xFF : 0;
	b[CELL_PAD_PCLASS_BTN_OFFSET_GUITAR_STRUM_DOWN] = strum_down(s.hat) ? 0xFF : 0;
	b[CELL_PAD_PCLASS_BTN_OFFSET_GUITAR_WHAMMYBAR]  = s.whammy;
	b[CELL_PAD_PCLASS_BTN_OFFSET_GUITAR_TILT_SENS]  = scale(s.tilt, RM_TILT_MIN, RM_TILT_MAX, 0x00, 0xFF);
	return CELL_OK;
}

/* ------------------------------------------------------------------------ */
/* Import redirection in the game's main executable                          */
/* ------------------------------------------------------------------------ */

/* sceLibStub entry (0x2C bytes). */
typedef struct {
	uint8_t  size;
	uint8_t  unk0;
	uint16_t version;
	uint16_t attr;
	uint16_t num_func;
	uint16_t num_var;
	uint16_t num_tlsvar;
	uint8_t  hash_info;
	uint8_t  hash_info_tls;
	uint8_t  reserved[2];
	uint32_t module_name;
	uint32_t func_nid;
	uint32_t func_table;   /* slots holding the OPD address of each import */
	uint32_t var_nid;
	uint32_t var_table;
	uint32_t tls_nid;
	uint32_t tls_table;
} lib_stub_t;

/* sys_process_prx_info (PT 0x6000000x segment of the executable). */
typedef struct {
	uint32_t size;
	uint32_t magic;
	uint32_t version;
	uint32_t sdk_version;
	uint32_t libent_start;
	uint32_t libent_end;
	uint32_t libstub_start;
	uint32_t libstub_end;
} proc_prx_info_t;

#define PRX_INFO_MAGIC 0x1B434CEC
#define EXEC_BASE      0x10000

typedef struct {
	uint32_t nid;
	void    *hook;
	uint32_t slot;       /* address patched, 0 if not found */
	uint32_t original;   /* OPD address that was there */
} import_hook_t;

/* FNID = first 4 bytes (LE) of SHA-1(name + PS3 suffix). */
static import_hook_t g_hooks[] = {
	{ 0x3AAAD464, (void *)hook_GetInfo,        0, 0 },  /* cellPadGetInfo */
	{ 0xA703A51D, (void *)hook_GetInfo2,       0, 0 },  /* cellPadGetInfo2 */
	{ 0x4CC9B68D, (void *)hook_PeriphGetInfo,  0, 0 },  /* cellPadPeriphGetInfo */
	{ 0x8A00F264, (void *)hook_PeriphGetData,  0, 0 },  /* cellPadPeriphGetData */
};
#define NUM_HOOKS (sizeof(g_hooks) / sizeof(g_hooks[0]))

static void write_u32(uint32_t addr, uint32_t value)
{
	if (ps3mapi_set_proc_mem(sys_process_getpid(), addr, &value, sizeof(value)) != 0)
		*(volatile uint32_t *)addr = value;
}

static const proc_prx_info_t *find_prx_info(void)
{
	const uint8_t *elf = (const uint8_t *)EXEC_BASE;
	if (elf[0] != 0x7F || elf[1] != 'E' || elf[2] != 'L' || elf[3] != 'F')
		return NULL;

	uint64_t phoff     = *(const uint64_t *)(elf + 0x20);
	uint16_t phentsize = *(const uint16_t *)(elf + 0x36);
	uint16_t phnum     = *(const uint16_t *)(elf + 0x38);

	for (uint16_t i = 0; i < phnum; i++) {
		const uint8_t *ph = elf + phoff + (uint32_t)i * phentsize;
		uint32_t type  = *(const uint32_t *)ph;
		uint64_t vaddr = *(const uint64_t *)(ph + 0x10);
		if ((type == 0x60000001 || type == 0x60000002) && vaddr) {
			const proc_prx_info_t *info = (const proc_prx_info_t *)(uint32_t)vaddr;
			if (info->magic == PRX_INFO_MAGIC)
				return info;
		}
	}
	return NULL;
}

static int patch_imports(void)
{
	const proc_prx_info_t *info = find_prx_info();
	if (info == NULL) {
		rm_log("prx info not found in executable");
		return 0;
	}

	int patched = 0;
	for (uint32_t p = info->libstub_start; p < info->libstub_end; ) {
		const lib_stub_t *stub = (const lib_stub_t *)p;
		if (stub->size == 0) break;
		const char *mod = (const char *)stub->module_name;
		if (mod && mod[0] == 's' && mod[1] == 'y' && mod[2] == 's' && mod[3] == '_' &&
		    mod[4] == 'i' && mod[5] == 'o' && mod[6] == '\0') {
			const uint32_t *nids  = (const uint32_t *)stub->func_nid;
			const uint32_t *slots = (const uint32_t *)stub->func_table;
			for (uint16_t f = 0; f < stub->num_func; f++) {
				for (uint32_t h = 0; h < NUM_HOOKS; h++) {
					if (nids[f] != g_hooks[h].nid || g_hooks[h].slot) continue;
					g_hooks[h].slot     = (uint32_t)&slots[f];
					g_hooks[h].original = slots[f];
					write_u32(g_hooks[h].slot, (uint32_t)g_hooks[h].hook);
					rm_logx("hooked nid", g_hooks[h].nid);
					patched++;
				}
			}
		}
		p += stub->size;
	}
	return patched;
}

static void unpatch_imports(void)
{
	for (uint32_t h = 0; h < NUM_HOOKS; h++) {
		if (g_hooks[h].slot) {
			write_u32(g_hooks[h].slot, g_hooks[h].original);
			g_hooks[h].slot = 0;
		}
	}
}

/* ------------------------------------------------------------------------ */
/* Profile selection                                                         */
/* ------------------------------------------------------------------------ */

#define CFG_PATH "/dev_hdd0/plugins/riffmaster.cfg"
#define SFO_PATH "/dev_bdvd/PS3_GAME/PARAM.SFO"

/* Returns 'g', 'r' or 0 (auto/missing) from the first non-comment "profile=" line. */
static char read_cfg_profile(char *buf, int n)
{
	for (int i = 0; i < n; ) {
		int end = i;
		while (end < n && buf[end] != '\n') end++;
		if (buf[i] != '#' && end - i >= 10 && rm_memifind(buf + i, 8, "profile="))
			return (char)rm_lower(buf[i + 8]) == 'r' ? 'r' :
			       (char)rm_lower(buf[i + 8]) == 'g' ? 'g' : 0;
		i = end + 1;
	}
	return 0;
}

static void select_profile(void)
{
	static char buf[4096];
	int n = rm_read_file(CFG_PATH, buf, sizeof(buf));
	char forced = n > 0 ? read_cfg_profile(buf, n) : 0;
	if (forced == 'r') {
		g_profile = &PROFILE_RB;
	} else if (forced == 'g') {
		g_profile = &PROFILE_GH;
	} else {
		/* auto: every Rock Band title has "Rock Band" in its PARAM.SFO TITLE. */
		n = rm_read_file(SFO_PATH, buf, sizeof(buf));
		g_profile = (n > 0 && rm_memifind(buf, (size_t)n, "rock band")) ? &PROFILE_RB : &PROFILE_GH;
	}
	rm_log(g_profile == &PROFILE_RB ? "profile: rb (0x12BA:0x0200)" : "profile: gh (0x12BA:0x0100)");
}

/* ------------------------------------------------------------------------ */
/* Module entry points                                                       */
/* ------------------------------------------------------------------------ */

static void riff_thread(uint64_t arg)
{
	(void)arg;
	cellSysmoduleLoadModule(CELL_SYSMODULE_FS);
	rm_log("riffmaster_game started");

	select_profile();
	rm_logx("imports patched", patch_imports());

	int32_t r = cellSysmoduleLoadModule(CELL_SYSMODULE_USBD);
	rm_logx("load USBD module", r);
	r = cellUsbdInit();
	if (r != CELL_OK && r != (int32_t)CELL_USBD_ERROR_ALREADY_INITIALIZED)
		rm_logx("cellUsbdInit failed", r);
	r = cellUsbdRegisterExtraLdd2(&g_ldd_ops, RM_VID, RM_PID_MIN, RM_PID_MAX);
	rm_logx("cellUsbdRegisterExtraLdd2", r);

	sys_ppu_thread_exit(0);
}

int riff_start(size_t args, void *argp)
{
	(void)args; (void)argp;
	sys_lwmutex_attribute_t attr;
	sys_lwmutex_attribute_initialize(attr);
	sys_lwmutex_create(&g_lock, &attr);
	g_running = true;

	sys_ppu_thread_create(&g_thread, riff_thread, 0, 1000, 0x4000,
	                      SYS_PPU_THREAD_CREATE_JOINABLE, "riffmaster_game");
	return SYS_PRX_RESIDENT;
}

int riff_stop(size_t args, void *argp)
{
	(void)args; (void)argp;
	uint64_t exit_code;
	g_running = false;
	sys_ppu_thread_join(g_thread, &exit_code);

	cellUsbdUnregisterExtraLdd(&g_ldd_ops);
	if (g_dev_id >= 0) rm_detach(g_dev_id);
	if (g_buf) { cellUsbdFreeMemory(g_buf); g_buf = NULL; }
	unpatch_imports();
	sys_lwmutex_destroy(&g_lock);
	return SYS_PRX_STOP_OK;
}
