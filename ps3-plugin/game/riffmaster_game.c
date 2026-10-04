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

#define RIFF_LOG_TAG "game"
/* Report how far the first log writes get (see riff_log_probe). */
static void riff_log_probe(unsigned point);
#define RIFF_LOG_PROBE(n) riff_log_probe(n)
#include "../common/util.h"

#ifndef USB_CLASS_HID
#define USB_CLASS_HID 0x03
#endif
#include "../common/ps3mapi.h"
#include "../common/status.h"

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

/* Counters for the heartbeat and for rate-limiting logs on hot paths. */
static uint32_t g_n_read_done, g_n_read_err, g_n_submit_err, g_n_bad_report;
static uint32_t g_n_reports, g_n_changes, g_n_inserts, g_n_insert_err;
static uint32_t g_n_get_info, g_n_get_info2, g_n_periph_info, g_n_periph_data;

static int32_t ldd_port(void)
{
	return g_ldd >= 0 ? cellPadLddGetPortNo(g_ldd) : -1;
}

/* ------------------------------------------------------------------------ */
/* Progress reports to the loader (see common/status.h)                      */
/* ------------------------------------------------------------------------ */

static riff_arg_t    g_arg;     /* from the loader, magic 0 if none was passed */
/* Initialized, so it lands in .data where the loader can find the signature. */
static riff_status_t g_status = { { RIFF_STATUS_SIG0, RIFF_STATUS_SIG1 } };

/* Copies g_status into the loader's memory. Doesn't log, so it works even if logging is broken. */
static void push_status(void)
{
	g_status.seq++;
	g_status.log_err = rm_log_err;
	g_status.profile = g_profile == &PROFILE_RB ? 'r' : 'g';
	if (g_arg.magic == RIFF_ARG_MAGIC)
		g_status.report_err = ps3mapi_set_proc_mem(g_arg.vsh_pid, g_arg.status_addr, &g_status, sizeof(g_status));
}

static void report(uint32_t step, int32_t result)
{
	g_status.step   = step;
	g_status.result = result;
	push_status();
	rm_logf("report step %u result 0x%x to loader pid 0x%x at 0x%08x: 0x%x",
	        step, result, g_arg.vsh_pid, g_arg.status_addr, g_status.report_err);
}

/* Records that startup reached a source line, with a result, for when it stops between reports. */
static void mark(uint32_t line, int32_t result)
{
	g_status.mark_line   = line;
	g_status.mark_result = result;
	push_status();
}

/* Only the first few log lines are probed; each probe costs a PS3MAPI call. */
#define LOG_PROBE_LINES 3
static uint32_t g_probed_lines;

static void riff_log_probe(unsigned point)
{
	if (g_probed_lines >= LOG_PROBE_LINES) return;
	g_status.log_probe = point;
	push_status();
	if (point == 9 || (point == 5 && rm_log_err != 0))
		g_probed_lines++;
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
	if (len < RM_OFF_FRETS + 1 || r[0] != RM_REPORT_ID) {
		if (rm_log_every(&g_n_bad_report, 5, 500)) {
			rm_logf("ignored report #%u: len %d, id 0x%02x", g_n_bad_report, len, len > 0 ? r[0] : 0);
			rm_log_hex("  bad report", r, len > 0 ? (size_t)len : 0);
		}
		return;
	}
	if (rm_log_every(&g_n_reports, 8, 0)) {
		if (g_n_reports == 1)
			report(RIFF_STEP_FIRST_REPORT, len);
		rm_logf("report #%u, len %d", g_n_reports, len);
		rm_log_hex("  report", r, (size_t)len);
	}

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

	/* Analog jitter is skipped here so the log tracks button presses. */
	bool digital = prev.frets != snap.frets || prev.hat != snap.hat ||
	               prev.btn6 != snap.btn6 || prev.tilted != snap.tilted;
	if (digital && rm_log_every(&g_n_changes, 300, 100))
		rm_logf("state #%u: frets 0x%02x hat %x btn6 0x%02x whammy 0x%02x tilt 0x%02x tilted %d",
		        g_n_changes, snap.frets, snap.hat, snap.btn6, snap.whammy, snap.tilt, snap.tilted);

	if (g_ldd < 0) {
		rm_logf("registering virtual pad: cellPadLddRegisterController()...");
		g_ldd = cellPadLddRegisterController();
		rm_logf("cellPadLddRegisterController returned 0x%x", g_ldd);
		if (g_ldd < 0) return;
		rm_logf("virtual pad port %d", ldd_port());
		changed = true;
	}
	if (changed) {
		CellPadData data;
		build_pad_data(&snap, &data);
		int32_t ins = cellPadLddDataInsert(g_ldd, &data);
		g_n_inserts++;
		if (ins != CELL_OK && rm_log_every(&g_n_insert_err, 10, 200))
			rm_logf("cellPadLddDataInsert failed 0x%x (failure #%u)", ins, g_n_insert_err);
	}
}

static void read_done(int32_t result, int32_t count, void *arg)
{
	(void)arg;
	if (rm_log_every(&g_n_read_done, 5, 0))
		rm_logf("read_done #%u: result 0x%x count %d", g_n_read_done, result, count);
	if (result == HC_CC_NOERR)
		handle_report(g_buf, count);
	else if (rm_log_every(&g_n_read_err, 20, 500))
		rm_logf("interrupt read error 0x%x count %d (error #%u)", result, count, g_n_read_err);
	if (g_running && g_intr_pipe >= 0)
		submit_read();
	else if (rm_log_every(&g_n_submit_err, 5, 500))
		rm_logf("not resubmitting read: running %d pipe %d", g_running, g_intr_pipe);
}

static void submit_read(void)
{
	int32_t r = cellUsbdInterruptTransfer(g_intr_pipe, g_buf, RM_REPORT_SIZE, read_done, NULL);
	if (r != CELL_OK && rm_log_every(&g_n_submit_err, 20, 500))
		rm_logf("cellUsbdInterruptTransfer failed 0x%x (failure #%u)", r, g_n_submit_err);
}

static void set_config_done(int32_t result, int32_t count, void *arg)
{
	(void)arg;
	rm_logf("set_config_done: result 0x%x count %d", result, count);
	if (result != HC_CC_NOERR) {
		rm_logf("SET_CONFIGURATION failed 0x%x", result);
		return;
	}
	rm_logf("opening interrupt pipe: dev %d ep %p", g_dev_id, g_intr_ep);
	g_intr_pipe = cellUsbdOpenPipe(g_dev_id, g_intr_ep);
	rm_logf("interrupt pipe %d", g_intr_pipe);
	if (g_intr_pipe >= 0) {
		rm_logf("submitting first interrupt read (%d bytes into %p)", RM_REPORT_SIZE, g_buf);
		submit_read();
	}
}

/* Raw descriptors start with bLength, so they can be dumped without knowing their type. */
static void log_descriptor(const char *label, const void *desc)
{
	if (desc == NULL) {
		rm_logf("%s: (none)", label);
		return;
	}
	rm_log_hex(label, desc, ((const uint8_t *)desc)[0]);
}

/* First interrupt-IN endpoint of the first HID interface, or NULL. */
static UsbEndpointDescriptor *find_hid_in_endpoint(int32_t dev_id, bool verbose)
{
	UsbInterfaceDescriptor *ifd = NULL;
	while ((ifd = (UsbInterfaceDescriptor *)cellUsbdScanStaticDescriptor(
	            dev_id, ifd, USB_DESCRIPTOR_TYPE_INTERFACE)) != NULL) {
		if (verbose) {
			rm_logf("  interface %p class 0x%02x", ifd, ifd->bInterfaceClass);
			log_descriptor("    interface desc", ifd);
		}
		if (ifd->bInterfaceClass != USB_CLASS_HID)
			continue;
		void *p = ifd;
		UsbEndpointDescriptor *ep;
		while ((ep = (UsbEndpointDescriptor *)cellUsbdScanStaticDescriptor(
		            dev_id, p, USB_DESCRIPTOR_TYPE_ENDPOINT)) != NULL) {
			if (verbose)
				rm_logf("    endpoint %p addr 0x%02x attr 0x%02x", ep, ep->bEndpointAddress, ep->bmAttributes);
			if ((ep->bEndpointAddress & 0x80) && (ep->bmAttributes & 0x03) == 0x03) {
				if (verbose) rm_logf("    -> using this interrupt IN endpoint");
				return ep;
			}
			p = ep;
		}
	}
	if (verbose) rm_logf("  no HID interrupt IN endpoint found");
	return NULL;
}

static int rm_probe(int32_t dev_id)
{
	rm_logf("probe: dev %d", dev_id);
	log_descriptor("  device desc", cellUsbdScanStaticDescriptor(dev_id, NULL, USB_DESCRIPTOR_TYPE_DEVICE));
	if (find_hid_in_endpoint(dev_id, true) == NULL) {
		rm_logf("probe: dev %d rejected", dev_id);
		return CELL_USBD_PROBE_FAILED;
	}
	rm_logf("probe: dev %d accepted", dev_id);
	return CELL_USBD_PROBE_SUCCEEDED;
}

static int rm_attach(int32_t dev_id)
{
	rm_logf("attach: dev %d", dev_id);
	UsbConfigurationDescriptor *cfg = (UsbConfigurationDescriptor *)
		cellUsbdScanStaticDescriptor(dev_id, NULL, USB_DESCRIPTOR_TYPE_CONFIGURATION);
	log_descriptor("  config desc", cfg);
	g_intr_ep = find_hid_in_endpoint(dev_id, false);
	rm_logf("  interrupt endpoint %p", g_intr_ep);
	if (cfg == NULL || g_intr_ep == NULL) {
		rm_logf("attach failed: missing config or endpoint descriptor");
		return CELL_USBD_ATTACH_FAILED;
	}

	if (g_buf == NULL) {
		void *mem = NULL;
		int32_t r = cellUsbdAllocateMemory(&mem, RM_REPORT_SIZE);
		rm_logf("  cellUsbdAllocateMemory(%d) returned 0x%x, mem %p", RM_REPORT_SIZE, r, mem);
		if (r != CELL_OK || mem == NULL) {
			rm_logf("attach failed: cellUsbdAllocateMemory");
			return CELL_USBD_ATTACH_FAILED;
		}
		g_buf = (uint8_t *)mem;
	}

	g_dev_id = dev_id;
	g_ctrl_pipe = cellUsbdOpenPipe(dev_id, NULL);
	rm_logf("  control pipe %d", g_ctrl_pipe);
	if (g_ctrl_pipe < 0) {
		rm_logf("attach failed: control pipe 0x%x", g_ctrl_pipe);
		return CELL_USBD_ATTACH_FAILED;
	}
	rm_logf("  SET_CONFIGURATION %d...", cfg->bConfigurationValue);
	int32_t r = cellUsbdSetConfiguration(g_ctrl_pipe, cfg->bConfigurationValue, set_config_done, NULL);
	rm_logf("  cellUsbdSetConfiguration returned 0x%x", r);
	report(RIFF_STEP_ATTACHED, r);
	return r == CELL_OK ? CELL_USBD_ATTACH_SUCCEEDED : CELL_USBD_ATTACH_FAILED;
}

static int rm_detach(int32_t dev_id)
{
	rm_logf("detach: dev %d (intr pipe %d, ctrl pipe %d, ldd %d)", dev_id, g_intr_pipe, g_ctrl_pipe, g_ldd);
	if (g_intr_pipe >= 0) cellUsbdClosePipe(g_intr_pipe);
	if (g_ctrl_pipe >= 0) cellUsbdClosePipe(g_ctrl_pipe);
	g_intr_pipe = g_ctrl_pipe = g_dev_id = -1;
	if (g_ldd >= 0) {
		int32_t r = cellPadLddUnregisterController(g_ldd);
		rm_logf("  cellPadLddUnregisterController returned 0x%x", r);
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

/*
 * cellPadGetInfo and CellPadInfo were dropped from newer SDKs (no stub to link
 * against), but older games such as GH3 still import them. Use the legacy
 * layout and call the game's original import through its saved OPD.
 */
#define LEGACY_MAX_PADS 127
typedef struct {
	uint32_t max_connect;
	uint32_t now_connect;
	uint32_t system_info;
	uint16_t vendor_id[LEGACY_MAX_PADS];
	uint16_t product_id[LEGACY_MAX_PADS];
	uint8_t  status[LEGACY_MAX_PADS];
} CellPadInfo;

typedef int32_t (*pad_get_info_fn)(CellPadInfo *info);
static uint32_t g_orig_get_info;  /* set by patch_imports */

/* Hooks run on the game's threads every frame: log the first calls, then every 1000th. */
#define HOOK_LOG_FIRST 5
#define HOOK_LOG_EVERY 1000

static int32_t hook_GetInfo(CellPadInfo *info)
{
	bool log = rm_log_every(&g_n_get_info, HOOK_LOG_FIRST, HOOK_LOG_EVERY);
	if (log) rm_logf("hook_GetInfo #%u: info %p, calling original OPD 0x%08x", g_n_get_info, info, g_orig_get_info);
	int32_t r = ((pad_get_info_fn)g_orig_get_info)(info);
	int32_t port = ldd_port();
	if (log) rm_logf("hook_GetInfo #%u: r 0x%x, now_connect %u, ldd port %d", g_n_get_info, r,
	                 r == CELL_OK ? info->now_connect : 0, port);
	if (r == CELL_OK && port >= 0 && port < CELL_PAD_MAX_PORT_NUM) {
		info->vendor_id[port]  = g_profile->vid;
		info->product_id[port] = g_profile->pid;
	}
	return r;
}

static int32_t hook_GetInfo2(CellPadInfo2 *info)
{
	bool log = rm_log_every(&g_n_get_info2, HOOK_LOG_FIRST, HOOK_LOG_EVERY);
	if (log) rm_logf("hook_GetInfo2 #%u: info %p", g_n_get_info2, info);
	int32_t r = cellPadGetInfo2(info);
	int32_t port = ldd_port();
	if (log) rm_logf("hook_GetInfo2 #%u: r 0x%x, now_connect %u, ldd port %d", g_n_get_info2, r,
	                 r == CELL_OK ? info->now_connect : 0, port);
	if (r == CELL_OK && port >= 0 && port < CELL_PAD_MAX_PORT_NUM) {
		info->device_type[port]       = CELL_PAD_DEV_TYPE_STANDARD;
		info->device_capability[port] = GUITAR_CAPABILITY;
	}
	return r;
}

static int32_t hook_PeriphGetInfo(CellPadPeriphInfo *info)
{
	bool log = rm_log_every(&g_n_periph_info, HOOK_LOG_FIRST, HOOK_LOG_EVERY);
	if (log) rm_logf("hook_PeriphGetInfo #%u: info %p", g_n_periph_info, info);
	int32_t r = cellPadPeriphGetInfo(info);
	int32_t port = ldd_port();
	if (log) rm_logf("hook_PeriphGetInfo #%u: r 0x%x, now_connect %u, ldd port %d", g_n_periph_info, r,
	                 r == CELL_OK ? info->now_connect : 0, port);
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
	bool log = rm_log_every(&g_n_periph_data, HOOK_LOG_FIRST, HOOK_LOG_EVERY);
	if (log) rm_logf("hook_PeriphGetData #%u: port %u, ldd port %d, data %p",
	                 g_n_periph_data, port_no, ldd_port(), data);
	if ((int32_t)port_no != ldd_port())
		return cellPadPeriphGetData(port_no, data);

	rm_state_t s;
	sys_lwmutex_lock(&g_lock, 0);
	s = g_state;
	sys_lwmutex_unlock(&g_lock);

	CellPadData pad;
	build_pad_data(&s, &pad);
	rm_memset(data, 0, sizeof(*data));
	rm_memcpy(data->button, pad.button, sizeof(pad.button));
	data->pclass_type    = CELL_PAD_PCLASS_TYPE_GUITAR;
	data->pclass_profile = GUITAR_PROFILE_BITS;
	data->len            = CELL_PAD_PCLASS_BTN_OFFSET_GUITAR_TILT_SENS + 1;

	uint16_t *b = data->button;
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
	uint32_t pid = sys_process_getpid();
	rm_logf("  write_u32 0x%08x <- 0x%08x (was 0x%08x), ps3mapi_set_proc_mem pid 0x%x...",
	        addr, value, *(volatile uint32_t *)addr, pid);
	int r = ps3mapi_set_proc_mem(pid, addr, &value, sizeof(value));
	rm_logf("  ps3mapi_set_proc_mem returned %d (0x%x)", r, r);
	if (r != 0) {
		rm_logf("  falling back to a direct store (faults if the page is read-only)...");
		*(volatile uint32_t *)addr = value;
	}
	rm_logf("  readback 0x%08x%s", *(volatile uint32_t *)addr,
	        *(volatile uint32_t *)addr == value ? "" : "  <- MISMATCH");
}

static const proc_prx_info_t *find_prx_info(void)
{
	const uint8_t *elf = (const uint8_t *)EXEC_BASE;
	rm_logf("reading ELF header at 0x%08x...", EXEC_BASE);
	rm_log_hex("  elf header", elf, 0x40);
	if (elf[0] != 0x7F || elf[1] != 'E' || elf[2] != 'L' || elf[3] != 'F') {
		rm_logf("no ELF magic at 0x%08x", EXEC_BASE);
		return NULL;
	}

	uint64_t phoff     = *(const uint64_t *)(elf + 0x20);
	uint16_t phentsize = *(const uint16_t *)(elf + 0x36);
	uint16_t phnum     = *(const uint16_t *)(elf + 0x38);
	rm_logf("phoff 0x%llx phentsize %u phnum %u", (unsigned long long)phoff, phentsize, phnum);

	for (uint16_t i = 0; i < phnum; i++) {
		const uint8_t *ph = elf + phoff + (uint32_t)i * phentsize;
		uint32_t type  = *(const uint32_t *)ph;
		uint64_t vaddr = *(const uint64_t *)(ph + 0x10);
		uint64_t memsz = *(const uint64_t *)(ph + 0x28);
		rm_logf("  phdr %u: type 0x%08x vaddr 0x%llx memsz 0x%llx", i, type,
		        (unsigned long long)vaddr, (unsigned long long)memsz);
		if ((type == 0x60000001 || type == 0x60000002) && vaddr) {
			const proc_prx_info_t *info = (const proc_prx_info_t *)(uint32_t)vaddr;
			rm_logf("  candidate prx info at %p: magic 0x%08x", info, info->magic);
			if (info->magic == PRX_INFO_MAGIC) {
				rm_logf("  prx info: sdk 0x%08x libent 0x%08x-0x%08x libstub 0x%08x-0x%08x",
				        info->sdk_version, info->libent_start, info->libent_end,
				        info->libstub_start, info->libstub_end);
				return info;
			}
		}
	}
	return NULL;
}

static int patch_imports(void)
{
	const proc_prx_info_t *info = find_prx_info();
	if (info == NULL) {
		rm_logf("prx info not found in executable");
		return 0;
	}

	int patched = 0;
	for (uint32_t p = info->libstub_start; p < info->libstub_end; ) {
		const lib_stub_t *stub = (const lib_stub_t *)p;
		if (stub->size == 0) {
			rm_logf("  stub at 0x%08x has size 0, stopping", p);
			break;
		}
		const char *mod = (const char *)stub->module_name;
		rm_logf("  stub 0x%08x size 0x%02x funcs %u vars %u module '%.40s'",
		        p, stub->size, stub->num_func, stub->num_var, mod ? mod : "(null)");
		if (mod && mod[0] == 's' && mod[1] == 'y' && mod[2] == 's' && mod[3] == '_' &&
		    mod[4] == 'i' && mod[5] == 'o' && mod[6] == '\0') {
			const uint32_t *nids  = (const uint32_t *)stub->func_nid;
			const uint32_t *slots = (const uint32_t *)stub->func_table;
			for (uint16_t f = 0; f < stub->num_func; f++) {
				rm_logf("    sys_io import %u: nid 0x%08x slot %p -> 0x%08x", f, nids[f], &slots[f], slots[f]);
				for (uint32_t h = 0; h < NUM_HOOKS; h++) {
					if (nids[f] != g_hooks[h].nid || g_hooks[h].slot) continue;
					g_hooks[h].slot     = (uint32_t)&slots[f];
					g_hooks[h].original = slots[f];
					if (g_hooks[h].hook == (void *)hook_GetInfo)
						g_orig_get_info = slots[f];
					rm_logf("    hooking nid 0x%08x: slot 0x%08x original OPD 0x%08x hook OPD %p",
					        g_hooks[h].nid, g_hooks[h].slot, g_hooks[h].original, g_hooks[h].hook);
					write_u32(g_hooks[h].slot, (uint32_t)g_hooks[h].hook);
					patched++;
				}
			}
		}
		p += stub->size;
	}
	for (uint32_t h = 0; h < NUM_HOOKS; h++)
		if (!g_hooks[h].slot)
			rm_logf("  nid 0x%08x not imported by the game, not hooked", g_hooks[h].nid);
	return patched;
}

static void unpatch_imports(void)
{
	for (uint32_t h = 0; h < NUM_HOOKS; h++) {
		if (g_hooks[h].slot) {
			rm_logf("unhooking nid 0x%08x", g_hooks[h].nid);
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
	rm_logf("%s: read %d bytes, forced profile '%c'", CFG_PATH, n, forced ? forced : '-');
	if (forced == 'r') {
		g_profile = &PROFILE_RB;
	} else if (forced == 'g') {
		g_profile = &PROFILE_GH;
	} else {
		/* auto: every Rock Band title has "Rock Band" in its PARAM.SFO TITLE. */
		n = rm_read_file(SFO_PATH, buf, sizeof(buf));
		bool rb = n > 0 && rm_memifind(buf, (size_t)n, "rock band");
		rm_logf("%s: read %d bytes, 'rock band' %s", SFO_PATH, n, rb ? "found" : "not found");
		g_profile = rb ? &PROFILE_RB : &PROFILE_GH;
	}
	rm_logf("profile: %s (0x%04x:0x%04x)", g_profile->name, g_profile->vid, g_profile->pid);
}

/* ------------------------------------------------------------------------ */
/* Module entry points                                                       */
/* ------------------------------------------------------------------------ */

/* Logged by the heartbeat so a freeze can be timed against the last line. */
static void log_stats(void)
{
	rm_logf("heartbeat: dev %d intr pipe %d ldd %d port %d | reads %u errs %u submit errs %u bad %u | "
	        "reports %u changes %u inserts %u insert errs %u | GetInfo %u GetInfo2 %u PeriphInfo %u PeriphData %u",
	        g_dev_id, g_intr_pipe, g_ldd, ldd_port(), g_n_read_done, g_n_read_err, g_n_submit_err,
	        g_n_bad_report, g_n_reports, g_n_changes, g_n_inserts, g_n_insert_err,
	        g_n_get_info, g_n_get_info2, g_n_periph_info, g_n_periph_data);
}

static void riff_thread(uint64_t arg)
{
	(void)arg;
	mark(__LINE__, 0);
	rm_logf("riffmaster_game thread started");
	report(RIFF_STEP_THREAD, 0);

	rm_logf("selecting profile...");
	select_profile();
	report(RIFF_STEP_PROFILE, 0);
	rm_logf("patching imports...");
	int patched = patch_imports();
	rm_logf("imports patched: %d of %d", patched, (int)NUM_HOOKS);
	report(RIFF_STEP_IMPORTS, patched);

	rm_logf("cellSysmoduleLoadModule(USBD)...");
	int32_t r = cellSysmoduleLoadModule(CELL_SYSMODULE_USBD);
	rm_logf("cellSysmoduleLoadModule(USBD) returned 0x%x", r);
	report(RIFF_STEP_USBD_MODULE, r);
	rm_logf("cellUsbdInit()...");
	r = cellUsbdInit();
	rm_logf("cellUsbdInit returned 0x%x%s", r,
	        r == (int32_t)CELL_USBD_ERROR_ALREADY_INITIALIZED ? " (already initialized)" : "");
	report(RIFF_STEP_USBD_INIT, r);
	rm_logf("cellUsbdRegisterExtraLdd2(vid 0x%04x, pid 0x%04x-0x%04x)...", RM_VID, RM_PID_MIN, RM_PID_MAX);
	r = cellUsbdRegisterExtraLdd2(&g_ldd_ops, RM_VID, RM_PID_MIN, RM_PID_MAX);
	rm_logf("cellUsbdRegisterExtraLdd2 returned 0x%x", r);
	report(RIFF_STEP_READY, r);
	rm_logf("setup done, heartbeat every 2s for 60s, then every 15s");

	/* Heartbeat: shows how long the game process kept running. */
	uint64_t start = sys_time_get_system_time();
	uint32_t ticks = 0;
	while (g_running) {
		sys_timer_usleep(100 * 1000);
		ticks++;
		uint64_t elapsed = (sys_time_get_system_time() - start) / 1000000;
		if (ticks % (elapsed < 60 ? 20 : 150) == 0)
			log_stats();
	}
	rm_logf("riffmaster_game thread exiting");
	sys_ppu_thread_exit(0);
}

/*
 * Copies the loader's riff_arg_t from addr if it is there. Reads through PS3MAPI
 * so a bad address returns an error instead of crashing module_start.
 */
static bool take_loader_arg(uint32_t addr)
{
	riff_arg_t a;
	if (addr < 0x10000 || (addr & 3) != 0)
		return false;
	if (ps3mapi_get_proc_mem(sys_process_getpid(), addr, &a, sizeof(a)) != 0 || a.magic != RIFF_ARG_MAGIC)
		return false;
	rm_memcpy(&g_arg, &a, sizeof(g_arg));
	return true;
}

int riff_start(size_t args, void *argp)
{
	/*
	 * Report to the loader before anything else, so it learns that module_start
	 * ran even if logging to a file fails or hangs. The loader's argument is
	 * freed when module_start returns, so keep a copy. Cobra passes it as the
	 * first parameter (args); argp held 0xc8 on hardware, and reading that
	 * crashed module_start. Check argp too, but only through take_loader_arg.
	 */
	g_status.start_args = (uint32_t)args;
	g_status.start_argp = (uint32_t)argp;
	int where = take_loader_arg((uint32_t)args) ? 0 : take_loader_arg((uint32_t)argp) ? 1 : 2;
	report(RIFF_STEP_MODULE_START, where);

	rm_logf("riff_start: module_start in pid 0x%x, args 0x%x argp %p", sys_process_getpid(), (unsigned)args, argp);
	rm_logf("riff_start: loader arg %s, loader pid 0x%x, status at 0x%08x",
	        g_arg.magic == RIFF_ARG_MAGIC ? "ok" : "missing", g_arg.vsh_pid, g_arg.status_addr);
	sys_lwmutex_attribute_t attr;
	sys_lwmutex_attribute_initialize(attr);
	int r = sys_lwmutex_create(&g_lock, &attr);
	mark(__LINE__, r);
	rm_logf("riff_start: sys_lwmutex_create returned 0x%x", r);
	g_running = true;

	r = sys_ppu_thread_create(&g_thread, riff_thread, 0, 1000, 0x4000,
	                          SYS_PPU_THREAD_CREATE_JOINABLE, "riffmaster_game");
	mark(__LINE__, r);
	rm_logf("riff_start: sys_ppu_thread_create returned 0x%x, returning RESIDENT", r);
	return SYS_PRX_RESIDENT;
}

int riff_stop(size_t args, void *argp)
{
	(void)args; (void)argp;
	uint64_t exit_code;
	rm_logf("riff_stop: stopping");
	log_stats();
	g_running = false;
	sys_ppu_thread_join(g_thread, &exit_code);

	rm_logf("riff_stop: cellUsbdUnregisterExtraLdd returned 0x%x", cellUsbdUnregisterExtraLdd(&g_ldd_ops));
	if (g_dev_id >= 0) rm_detach(g_dev_id);
	if (g_buf) { cellUsbdFreeMemory(g_buf); g_buf = NULL; }
	unpatch_imports();
	sys_lwmutex_destroy(&g_lock);
	rm_logf("riff_stop: done");
	return SYS_PRX_STOP_OK;
}
