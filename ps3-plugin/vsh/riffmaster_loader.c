/*
 * riffmaster_loader.sprx - VSH plugin (add to /dev_hdd0/boot_plugins.txt).
 *
 * Asks VSH whether a game is running (like webMAN MOD does) and injects
 * riffmaster_game.sprx into it through Cobra's PS3MAPI, so the Riffmaster works
 * in every game without manual steps.
 */

#include <stdint.h>
#include <stdbool.h>
#include <sys/prx.h>
#include <sys/ppu_thread.h>
#include <sys/timer.h>
#include <sys/process.h>

#define RIFF_LOG_TAG "loader"
#include "../common/util.h"
#include "../common/ps3mapi.h"
#include "../common/status.h"

SYS_MODULE_INFO(riffmaster_loader, 0, 1, 0);
SYS_MODULE_START(loader_start);
SYS_MODULE_STOP(loader_stop);

#define GAME_PLUGIN_PATH  "/dev_hdd0/plugins/riffmaster_game.sprx"
#define POLL_SECONDS      2
/* Give the game time to call cellPadInit before the plugin registers its pad. */
#define INJECT_DELAY_SEC  8
/* After injecting, log a heartbeat every second for this long to timestamp a freeze. */
#define WATCH_SECONDS     90
/* Otherwise, log a heartbeat every this many polls. */
#define HEARTBEAT_POLLS   15
/*
 * The loader stays loaded across games (seen on hardware), but it can be
 * loaded again in the same boot, e.g. by unloading and reloading it with
 * webMAN. If the system has been up for less than this, treat it as a fresh
 * boot and start a new log; otherwise append.
 */
#define FRESH_BOOT_US     (90ULL * 1000000)

static volatile bool g_running;
static sys_ppu_thread_t g_thread;
static bool g_log_started;

static uint64_t uptime_sec(void)
{
	return sys_time_get_system_time() / 1000000;
}

static void begin_log(void)
{
	if (g_log_started) return;
	g_log_started = true;

	uint64_t up = sys_time_get_system_time();
	if (up < FRESH_BOOT_US) {
		rm_log_rotate();
		rm_logf("=== new boot, riffmaster_loader starting (previous log: %s) ===", RIFF_LOG_OLD_PATH);
	} else {
		rm_logf("=== riffmaster_loader starting again in the same boot, appending ===");
	}
}

/* ------------------------------------------------------------------------ */
/* VSH exports and XMB notifications                                         */
/* ------------------------------------------------------------------------ */

/* Wait up to this long for the XMB to appear before showing the "loaded" notification anyway. */
#define XMB_WAIT_SEC      30
#define NOTIFY_MAX        160

#define NID_VSHTASK_NOTIFY      0xA02D46E7  /* vshtask: (int, const char *msg) */
#define NID_PAF_VIEW_FIND       0xF21655F3  /* paf View_Find: (const char *plugin) -> view or 0 */
#define NID_VSHMAIN_RUNNING_MODE 0xEB757101 /* vshmain GetCooperationMode: 0 on the XMB, else in a game */
#define NID_VSHMAIN_GAME_PID    0x0624D3AE  /* vshmain GetGameProcessID: the running game's pid */

typedef int      (*vshtask_notify_fn)(int unk, const char *msg);
typedef uint32_t (*view_find_fn)(const char *plugin);
typedef int32_t  (*running_mode_fn)(void);
typedef uint32_t (*game_pid_fn)(void);

static vshtask_notify_fn g_vshtask_notify;
static view_find_fn      g_view_find;
static running_mode_fn   g_running_mode;
static game_pid_fn       g_game_pid;

static bool rm_streq(const char *a, const char *b)
{
	while (*a && *a == *b) { a++; b++; }
	return *a == *b;
}

/*
 * Finds a function exported by a VSH module and returns its OPD, or NULL.
 * Same lookup as webMAN MOD's getNIDfunc: the VSH executable keeps a
 * NULL-terminated list of export stubs at *(0x1008C) + 0x984.
 */
static void *vsh_export(const char *lib, uint32_t fnid)
{
	uint32_t table = *(volatile uint32_t *)0x1008C + 0x984;
	for (; *(volatile uint32_t *)table; table += 4) {
		const uint8_t *stub = (const uint8_t *)*(volatile uint32_t *)table;
		const char *name = (const char *)*(const uint32_t *)(stub + 0x10);
		if (name == NULL || !rm_streq(name, lib)) continue;
		const uint32_t *nids = (const uint32_t *)*(const uint32_t *)(stub + 0x14);
		const uint32_t *opds = (const uint32_t *)*(const uint32_t *)(stub + 0x18);
		uint16_t count = *(const uint16_t *)(stub + 6);
		for (uint16_t i = 0; i < count; i++)
			if (nids[i] == fnid) return (void *)opds[i];
	}
	return NULL;
}

static void resolve_vsh_exports(void)
{
	g_vshtask_notify = (vshtask_notify_fn)vsh_export("vshtask", NID_VSHTASK_NOTIFY);
	g_view_find      = (view_find_fn)vsh_export("paf", NID_PAF_VIEW_FIND);
	g_running_mode   = (running_mode_fn)vsh_export("vshmain", NID_VSHMAIN_RUNNING_MODE);
	g_game_pid       = (game_pid_fn)vsh_export("vshmain", NID_VSHMAIN_GAME_PID);
	rm_logf("vsh exports: vshtask_notify %p, View_Find %p, GetCooperationMode %p, GetGameProcessID %p",
	        g_vshtask_notify, g_view_find, g_running_mode, g_game_pid);
}

static bool xmb_ready(void)
{
	return g_view_find && g_view_find("explore_plugin") != 0;
}

/*
 * notifications= in riffmaster.cfg: which XMB notifications to show. Every
 * message is logged either way.
 */
enum {
	NOTIFY_OFF     = 0,
	NOTIFY_PROBLEM = 1,  /* only failures */
	NOTIFY_ALL     = 2,  /* also progress: loaded, game found, guitar connected (the default) */
};
static int g_notify_level = NOTIFY_ALL;

/* Shows an XMB notification (if notifications= allows its level) and logs it. */
static void notify(int level, const char *fmt, ...)
{
	char msg[NOTIFY_MAX];
	rm_buf_t b = { msg, 0, sizeof(msg) - 1 };
	va_list ap;
	va_start(ap, fmt);
	rm_vformat(&b, fmt, ap);
	va_end(ap);
	msg[b.n] = '\0';

	bool show = g_notify_level >= level;
	rm_logf("notify: '%s'%s", msg, !show ? "  (not shown, notifications disabled for this level)" :
	        g_vshtask_notify ? "" : "  <- vshtask_notify not found, not shown");
	if (show && g_vshtask_notify)
		g_vshtask_notify(0, msg);
}

/* ------------------------------------------------------------------------ */
/* Game plugin progress (see common/status.h)                                */
/* ------------------------------------------------------------------------ */

/* If the game plugin hasn't finished setup this long after injection, say where it stopped. */
#define REPORT_TIMEOUT_SEC 20

/* Written by the game plugin through PS3MAPI SET_PROC_MEM. */
static volatile riff_status_t g_game_status;
static uint32_t g_seen_seq;
static uint64_t g_report_deadline;  /* 0 once setup finished or the timeout was reported */
static uint32_t g_injected_pid;
static int32_t g_liblv2_id;  /* liblv2's module ID in the game, set by log_game_modules */
static int32_t log_game_modules(uint32_t pid);
static void dump_game_imports(uint32_t pid, int32_t prx_id);
static void peek_game_status(uint32_t pid, int32_t prx_id);

static const char *const STEP_NAMES[RIFF_STEP_COUNT] = {
	"none", "module_start", "thread started", "profile selected", "imports hooked",
	"USBD module loaded", "cellUsbdInit", "setup done", "guitar attached", "first input report",
};

static const char *step_name(uint32_t step)
{
	return step < RIFF_STEP_COUNT ? STEP_NAMES[step] : "?";
}

/* How much of the game plugin's log buffer has been copied to the log file. */
static uint32_t g_game_log_addr, g_game_log_read;

static void reset_game_status(void)
{
	rm_memset((void *)&g_game_status, 0, sizeof(g_game_status));
	g_seen_seq = 0;
	g_report_deadline = 0;
	g_game_log_addr = g_game_log_read = 0;
}

/*
 * Copies new lines from the game plugin's log buffer (riff_logbuf_t in its
 * memory) into the log file. The lines already carry their timestamp and the
 * "game" tag, so they can land a little after loader lines from the same time.
 */
static void drain_game_log(uint32_t pid)
{
	static char buf[4096];
	uint32_t addr = g_game_status.logbuf_addr, write_pos;
	if (addr == 0 || pid == 0) return;
	if (addr != g_game_log_addr) {
		g_game_log_addr = addr;
		g_game_log_read = 0;
	}
	if (ps3mapi_get_proc_mem(pid, addr, &write_pos, sizeof(write_pos)) != 0) return;
	if (write_pos - g_game_log_read > RIFF_LOGBUF_SIZE) {
		rm_logf("game log: %u bytes lost, the game plugin logged faster than the loader copied",
		        write_pos - g_game_log_read - RIFF_LOGBUF_SIZE);
		g_game_log_read = write_pos - RIFF_LOGBUF_SIZE;
	}
	while (g_game_log_read != write_pos) {
		uint32_t off = g_game_log_read % RIFF_LOGBUF_SIZE;
		uint32_t n = write_pos - g_game_log_read;
		if (n > RIFF_LOGBUF_SIZE - off) n = RIFF_LOGBUF_SIZE - off;
		if (n > sizeof(buf)) n = sizeof(buf);
		if (ps3mapi_get_proc_mem(pid, addr + offsetof(riff_logbuf_t, data) + off, buf, n) != 0) {
			rm_logf("game log: reading the game plugin's log buffer failed");
			return;
		}
		rm_log_write(buf, n);
		g_game_log_read += n;
	}
}

/* Logs new progress from the game plugin and shows the notifications that matter. */
static void check_game_status(void)
{
	riff_status_t s;
	rm_memcpy(&s, (const void *)&g_game_status, sizeof(s));
	if (s.seq != g_seen_seq) {
		g_seen_seq = s.seq;
		rm_logf("game plugin: step %u (%s), result 0x%x, profile %c, mark line %u result 0x%x, log buffer 0x%08x",
		        s.step, step_name(s.step), s.result, s.profile ? s.profile : '-',
		        s.mark_line, s.mark_result, s.logbuf_addr);
		if (s.step == RIFF_STEP_READY) {
			g_report_deadline = 0;
			const char *guitar = s.profile == 'r' ? "Rock Band" : "Guitar Hero";
			if (s.result == 0)
				notify(NOTIFY_ALL, "Riffmaster plugin loaded (%s guitar)", guitar);
			else
				notify(NOTIFY_PROBLEM, "Riffmaster plugin loaded, but USB setup failed (0x%x)", s.result);
		} else if (s.step == RIFF_STEP_ATTACHED) {
			if (s.result == 0)
				notify(NOTIFY_ALL, "Riffmaster guitar connected");
			else
				notify(NOTIFY_PROBLEM, "Riffmaster guitar attach failed (0x%x)", s.result);
		}
	}
	if (g_report_deadline && uptime_sec() >= g_report_deadline) {
		g_report_deadline = 0;
		int32_t prx_id = log_game_modules(g_injected_pid);
		if (prx_id) {
			peek_game_status(g_injected_pid, prx_id);
			dump_game_imports(g_injected_pid, prx_id);
		}
		if (s.seq == 0) {
			rm_logf("game plugin sent no progress report within %ds of injection", REPORT_TIMEOUT_SEC);
			notify(NOTIFY_PROBLEM, "Riffmaster: guitar plugin didn't report back");
		} else {
			rm_logf("game plugin stopped at step %u (%s), result 0x%x", s.step, step_name(s.step), s.result);
			notify(NOTIFY_PROBLEM, "Riffmaster: plugin stopped after '%s' (0x%x)", step_name(s.step), s.result);
		}
	}
}

/* ------------------------------------------------------------------------ */
/* Game detection and injection                                              */
/* ------------------------------------------------------------------------ */

/*
 * debug_stage in riffmaster.cfg turns the loader's work on one step at a time,
 * to find which step freezes the console. Each stage adds to the one before.
 */
enum {
	STAGE_IDLE   = 0,  /* log and heartbeat only: no VSH or PS3MAPI calls, no notifications */
	STAGE_DETECT = 1,  /* + XMB notifications and game detection, but don't inject */
	STAGE_INJECT = 2,  /* + inject the game plugin (normal operation, the default) */
};

static int  g_stage = STAGE_INJECT;
/* trace=1 in riffmaster.cfg logs before and after every VSH and PS3MAPI call in the poll loop. */
static bool g_trace;

static void load_cfg(void)
{
	static char buf[4096];
	int n = rm_read_file(RIFF_CFG_PATH, buf, sizeof(buf));
	if (n > 0) {
		g_stage = rm_cfg_int(buf, n, "debug_stage", STAGE_INJECT);
		g_trace = rm_cfg_int(buf, n, "trace", 0) != 0;
		g_notify_level = rm_cfg_int(buf, n, "notifications", NOTIFY_ALL);
	}
	if (g_stage > STAGE_INJECT) g_stage = STAGE_INJECT;
	if (g_notify_level > NOTIFY_ALL) g_notify_level = NOTIFY_ALL;
	rm_logf("%s: read %d bytes, debug_stage %d%s, trace %d, notifications %d", RIFF_CFG_PATH, n, g_stage,
	        g_stage == STAGE_INJECT ? " (normal)" : "", g_trace, g_notify_level);
}

/*
 * Returns the running game's pid, or 0 on the XMB. Asks VSH directly, as
 * webMAN MOD does: listing processes through PS3MAPI (Cobra syscall 8) hung
 * the whole console when it ran while a game was starting.
 */
static uint32_t game_pid(void)
{
	if (g_running_mode == NULL || g_game_pid == NULL) return 0;
	if (g_trace) rm_logf("trace: GetCooperationMode...");
	int32_t mode = g_running_mode();
	if (g_trace) rm_logf("trace: GetCooperationMode returned %d", mode);
	if (mode == 0) return 0;
	if (g_trace) rm_logf("trace: GetGameProcessID...");
	uint32_t pid = g_game_pid();
	if (g_trace) rm_logf("trace: GetGameProcessID returned 0x%08x", pid);
	return pid;
}

static void sleep_while_running(uint32_t seconds)
{
	for (uint32_t i = 0; i < seconds * 10 && g_running; i++)
		sys_timer_usleep(100 * 1000);
}

static void log_game_plugin_file(void)
{
	CellFsStat st;
	int r = rm_fs_stat(GAME_PLUGIN_PATH, &st);
	if (r == 0)
		rm_logf("game plugin %s present, %llu bytes", GAME_PLUGIN_PATH, (unsigned long long)st.st_size);
	else
		rm_logf("game plugin %s NOT accessible: stat 0x%x", GAME_PLUGIN_PATH, r);
}

/*
 * Logs the modules loaded in the game process, to show whether the game plugin
 * is actually in there. Only runs after injection, when the game is already up.
 */
static int32_t log_game_modules(uint32_t pid)
{
	int32_t ours_id = 0;
	static int32_t ids[PS3MAPI_MAX_MODULES];
	static char name[64];
	bool found = false;

	rm_memset(ids, 0, sizeof(ids));
	rm_logf("listing modules in pid 0x%08x: ps3mapi_get_proc_modules...", pid);
	int r = ps3mapi_get_proc_modules(pid, ids);
	rm_logf("ps3mapi_get_proc_modules returned 0x%x", r);
	if (r != 0) return 0;
	for (int i = 0; i < PS3MAPI_MAX_MODULES && ids[i]; i++) {
		rm_memset(name, 0, sizeof(name));
		r = ps3mapi_get_proc_module_name(pid, ids[i], name);
		bool ours = r == 0 && rm_memifind(name, rm_strlen(name), "riffmaster");
		found |= ours;
		if (ours) ours_id = ids[i];
		if (r == 0 && rm_streq(name, "liblv2")) g_liblv2_id = ids[i];
		rm_logf("  module 0x%08x name_r 0x%x '%s'%s", ids[i], r, name, ours ? "  <- game plugin" : "");
	}
	rm_logf("game plugin %s loaded in the game process", found ? "IS" : "is NOT");
	return ours_id;
}

#define MAX_SEGS 8

/* Fills segs[MAX_SEGS] with a module's segments in the game process. */
static int get_segments(uint32_t pid, int32_t prx_id, sys_prx_segment_info_t *segs)
{
	static char filename[SYS_PRX_MODULE_FILENAME_SIZE];
	sys_prx_module_info_t info;
	rm_memset(&info, 0, sizeof(info));
	rm_memset(segs, 0, MAX_SEGS * sizeof(*segs));
	info.size = sizeof(info);
	info.segments = segs;
	info.segments_num = MAX_SEGS;
	info.filename = filename;
	info.filename_size = sizeof(filename);
	int r = ps3mapi_get_proc_module_segments(pid, prx_id, &info);
	rm_logf("ps3mapi_get_proc_module_segments(prx 0x%08x) returned 0x%x", prx_id, r);
	return r;
}

static bool in_segments(uint32_t addr, const sys_prx_segment_info_t *segs)
{
	for (int i = 0; i < MAX_SEGS; i++)
		if (segs[i].memsz && addr >= segs[i].base && addr - segs[i].base < segs[i].memsz)
			return true;
	return false;
}

/*
 * Reads the game plugin's own riff_status_t out of the game's memory, found by
 * its signature in the module's segments. Shows whether module_start ran even
 * when its reports never arrived.
 */
static void peek_game_status(uint32_t pid, int32_t prx_id)
{
	static sys_prx_segment_info_t segs[MAX_SEGS];
	static uint32_t chunk[1024];

	rm_logf("reading game plugin segments...");
	if (get_segments(pid, prx_id, segs) != 0) return;

	for (uint32_t s = 0; s < MAX_SEGS; s++) {
		uint32_t base = (uint32_t)segs[s].base, size = (uint32_t)segs[s].memsz;
		if (size == 0) continue;
		rm_logf("  segment %u: base 0x%08x memsz 0x%x type %llu", s, base, size,
		        (unsigned long long)segs[s].type);
		if (size > 0x40000) size = 0x40000;
		for (uint32_t off = 0; off < size; off += sizeof(chunk)) {
			uint32_t n = size - off < sizeof(chunk) ? size - off : sizeof(chunk);
			if (ps3mapi_get_proc_mem(pid, base + off, chunk, n) != 0) {
				rm_logf("  read at 0x%08x failed, skipping the rest of this segment", base + off);
				break;
			}
			for (uint32_t i = 0; i + sizeof(riff_status_t) / 4 <= n / 4; i++) {
				if (chunk[i] != RIFF_STATUS_SIG0 || chunk[i + 1] != RIFF_STATUS_SIG1) continue;
				/* A match near the end of the chunk may run past it, so read it whole. */
				riff_status_t st;
				if (ps3mapi_get_proc_mem(pid, base + off + i * 4, &st, sizeof(st)) != 0) continue;
				rm_logf("  game plugin status at 0x%08x: seq %u, step %u (%s), result 0x%x, report error 0x%x,"
				        " module_start args 0x%08x argp 0x%08x, mark line %u result 0x%x, log buffer 0x%08x",
				        base + off + i * 4, st.seq, st.step, step_name(st.step), st.result,
				        st.report_err, st.start_args, st.start_argp, st.mark_line, st.mark_result, st.logbuf_addr);
				if (st.seq == 0)
					rm_logf("  -> module_start never ran (or never reached its first report)");
				return;
			}
		}
	}
	rm_logf("  game plugin status signature not found in its segments");
}

/* sceLibStub entry (0x2C bytes), as in the game plugin. */
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
	uint32_t func_table;
	uint32_t var_nid;
	uint32_t var_table;
	uint32_t tls_nid;
	uint32_t tls_table;
} lib_stub_t;

#define MAX_STUB_FUNCS 64

/*
 * Logs where each function the game plugin imports points. A linked import
 * points at the exporting library's code (liblv2 for sysPrxForUser); an
 * unlinked one still points into riffmaster_game itself. Finds the import
 * table through the module info that SYS_MODULE_INFO puts in the module
 * ({attributes, version, name[28], toc, exports start/end, imports start/end}).
 */
static void dump_game_imports(uint32_t pid, int32_t prx_id)
{
	static sys_prx_segment_info_t ours[MAX_SEGS], lv2[MAX_SEGS];
	static uint8_t buf[0x10000];
	static const char name[] = "riffmaster_game";
	static uint32_t nids[MAX_STUB_FUNCS], slots[MAX_STUB_FUNCS];

	rm_logf("checking the game plugin's imports...");
	if (get_segments(pid, prx_id, ours) != 0) return;
	if (g_liblv2_id == 0 || get_segments(pid, g_liblv2_id, lv2) != 0)
		rm_memset(lv2, 0, sizeof(lv2));
	for (int i = 0; i < MAX_SEGS; i++)
		if (lv2[i].memsz)
			rm_logf("  liblv2 segment %d: 0x%08x-0x%08x", i, (uint32_t)lv2[i].base,
			        (uint32_t)(lv2[i].base + lv2[i].memsz));

	uint32_t imp_start = 0, imp_end = 0;
	for (int s = 0; s < MAX_SEGS && !imp_start; s++) {
		uint32_t base = (uint32_t)ours[s].base, size = (uint32_t)ours[s].memsz;
		if (size == 0) continue;
		if (size > sizeof(buf)) size = sizeof(buf);
		if (ps3mapi_get_proc_mem(pid, base, buf, size) != 0) {
			rm_logf("  reading segment %d failed", s);
			continue;
		}
		for (uint32_t i = 4; i + sizeof(name) + 48 <= size; i += 4) {
			bool match = true;
			for (uint32_t j = 0; j < sizeof(name) && match; j++)
				match = buf[i + j] == (uint8_t)name[j];
			if (!match) continue;
			const uint8_t *mi = buf + i - 4;
			uint32_t start = *(const uint32_t *)(mi + 44), end = *(const uint32_t *)(mi + 48);
			rm_logf("  module info candidate at 0x%08x: toc 0x%08x imports 0x%08x-0x%08x", base + i - 4,
			        *(const uint32_t *)(mi + 32), start, end);
			if (end > start && (end - start) % sizeof(lib_stub_t) == 0 && end - start <= 64 * sizeof(lib_stub_t)) {
				imp_start = start;
				imp_end = end;
				break;
			}
		}
	}
	if (!imp_start) {
		rm_logf("  module info not found, can't check imports");
		return;
	}

	uint32_t linked = 0, unlinked = 0, other = 0;
	for (uint32_t p = imp_start; p < imp_end; p += sizeof(lib_stub_t)) {
		lib_stub_t st;
		char lib[32];
		rm_memset(lib, 0, sizeof(lib));
		if (ps3mapi_get_proc_mem(pid, p, &st, sizeof(st)) != 0 ||
		    ps3mapi_get_proc_mem(pid, st.module_name, lib, sizeof(lib) - 1) != 0) {
			rm_logf("  stub 0x%08x: read failed", p);
			continue;
		}
		uint32_t n = st.num_func < MAX_STUB_FUNCS ? st.num_func : MAX_STUB_FUNCS;
		rm_logf("  imports from '%s': %u functions", lib, st.num_func);
		if (n == 0) continue;
		if (ps3mapi_get_proc_mem(pid, st.func_nid, nids, n * 4) != 0 ||
		    ps3mapi_get_proc_mem(pid, st.func_table, slots, n * 4) != 0) {
			rm_logf("    reading NID/slot tables failed");
			continue;
		}
		for (uint32_t f = 0; f < n; f++) {
			const char *where;
			if (in_segments(slots[f], ours))     { where = "UNLINKED (points into riffmaster_game)"; unlinked++; }
			else if (in_segments(slots[f], lv2)) { where = "liblv2"; linked++; }
			else                                 { where = "another library"; other++; }
			rm_logf("    nid 0x%08x slot 0x%08x -> 0x%08x  %s", nids[f], st.func_table + f * 4, slots[f], where);
		}
	}
	rm_logf("  imports: %u point into liblv2, %u into other libraries, %u UNLINKED", linked, other, unlinked);
}

static void inject(uint32_t pid)
{
	log_game_plugin_file();
	reset_game_status();
	riff_arg_t arg = { RIFF_ARG_MAGIC, (uint32_t)sys_process_getpid(), (uint32_t)&g_game_status };
	rm_logf("calling ps3mapi_load_proc_module(pid 0x%08x, %s), arg: loader pid 0x%x, status at 0x%08x...",
	        pid, GAME_PLUGIN_PATH, arg.vsh_pid, arg.status_addr);
	int r = ps3mapi_load_proc_module(pid, GAME_PLUGIN_PATH, &arg, sizeof(arg));
	rm_logf("ps3mapi_load_proc_module returned %d (0x%x)%s", r, r, r == 0 ? "" : "  <- FAILED");
	if (r != 0) {
		notify(NOTIFY_PROBLEM, "Riffmaster: failed to load guitar plugin (0x%x)", r);
	} else {
		g_injected_pid = pid;
		g_report_deadline = uptime_sec() + REPORT_TIMEOUT_SEC;
	}
	check_game_status();
}

static void loader_thread(uint64_t arg)
{
	(void)arg;
	uint32_t handled_pid = 0;  /* game already injected (or skipped, below STAGE_INJECT) */
	uint64_t watch_until = 0;
	uint32_t polls = 0;
	bool announce_pending = true;
	uint64_t announce_deadline;

	begin_log();
	rm_logf("loader thread running, uptime %llus, poll %ds, inject delay %ds",
	        uptime_sec(), POLL_SECONDS, INJECT_DELAY_SEC);
	load_cfg();
	log_game_plugin_file();
	if (g_stage >= STAGE_DETECT) {
		resolve_vsh_exports();
		if (g_running_mode == NULL || g_game_pid == NULL)
			rm_logf("game detection exports not found, the loader can't detect games");
	}
	announce_deadline = uptime_sec() + XMB_WAIT_SEC;

	while (g_running) {
		uint32_t pid = 0;
		if (g_stage >= STAGE_DETECT) {
			/* Notifications sent before the XMB is up are lost, so wait for it (or give up waiting). */
			if (announce_pending && (xmb_ready() || uptime_sec() >= announce_deadline)) {
				if (g_running_mode == NULL || g_game_pid == NULL)
					notify(NOTIFY_PROBLEM, "Riffmaster loader: can't detect games on this firmware");
				else if (g_stage >= STAGE_INJECT)
					notify(NOTIFY_ALL, "Riffmaster loader loaded");
				else
					notify(NOTIFY_ALL, "Riffmaster loader loaded (debug stage %d)", g_stage);
				announce_pending = false;
			}
			check_game_status();
			drain_game_log(g_injected_pid);

			if (g_trace) rm_logf("trace: poll #%u", polls);
			pid = game_pid();
		}

		if (pid && pid != handled_pid) {
			handled_pid = pid;
			if (g_stage < STAGE_INJECT) {
				rm_logf("game process found: pid 0x%08x, not injecting (debug_stage %d)", pid, g_stage);
				notify(NOTIFY_ALL, "Riffmaster: game found (debug stage %d, not loading the plugin)", g_stage);
			} else {
				rm_logf("game process found: pid 0x%08x, waiting %ds before injecting", pid, INJECT_DELAY_SEC);
				notify(NOTIFY_ALL, "Riffmaster: game found, loading guitar plugin in %ds", INJECT_DELAY_SEC);
				sleep_while_running(INJECT_DELAY_SEC);
				if (!g_running) {
					rm_logf("stop requested during inject delay");
					break;
				}
				uint32_t now = game_pid();
				if (now == pid)
					inject(pid);
				else
					rm_logf("game pid changed to 0x%08x before injection, skipping 0x%08x", now, pid);
			}
			watch_until = uptime_sec() + WATCH_SECONDS;
			rm_logf("heartbeat every 1s for the next %ds", WATCH_SECONDS);
		} else if (!pid && handled_pid) {
			rm_logf("game pid 0x%08x is gone, waiting for the next game", handled_pid);
			handled_pid = 0;
			g_injected_pid = 0;
			watch_until = 0;
		}

		polls++;
		bool watching = uptime_sec() < watch_until;
		if (watching || polls % HEARTBEAT_POLLS == 0)
			rm_logf("heartbeat: uptime %llus, stage %d, game pid 0x%08x, handled pid 0x%08x",
			        uptime_sec(), g_stage, pid, handled_pid);
		sleep_while_running(watching ? 1 : POLL_SECONDS);
	}
	rm_logf("loader thread exiting");
	sys_ppu_thread_exit(0);
}

int loader_start(size_t args, void *argp)
{
	(void)args; (void)argp;
	g_running = true;
	/* 16 KB stack: vshtask_notify runs on this thread. */
	int r = sys_ppu_thread_create(&g_thread, loader_thread, 0, 3000, 0x4000,
	                              SYS_PPU_THREAD_CREATE_JOINABLE, "riffmaster_loader");
	if (r != 0) {
		begin_log();
		rm_logf("sys_ppu_thread_create failed: 0x%x", r);
	}
	return SYS_PRX_RESIDENT;
}

int loader_stop(size_t args, void *argp)
{
	(void)args; (void)argp;
	uint64_t exit_code;
	rm_logf("loader_stop called");
	g_running = false;
	sys_ppu_thread_join(g_thread, &exit_code);
	rm_logf("loader_stop done");
	return SYS_PRX_STOP_OK;
}
