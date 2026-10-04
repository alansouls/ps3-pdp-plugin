/*
 * riffmaster_loader.sprx - VSH plugin (add to /dev_hdd0/boot_plugins.txt).
 *
 * Watches for a game process and injects riffmaster_game.sprx into it through
 * Cobra's PS3MAPI, so the Riffmaster works in every game without manual steps.
 */

#include <stdint.h>
#include <stdbool.h>
#include <sys/prx.h>
#include <sys/ppu_thread.h>
#include <sys/timer.h>

#define RIFF_LOG_TAG "loader"
#include "../common/util.h"
#include "../common/ps3mapi.h"

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
 * VSH plugins also load again when returning from a game, without a reboot.
 * If the system has been up for less than this, treat it as a fresh boot.
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
/* XMB notifications                                                         */
/* ------------------------------------------------------------------------ */

/* Wait up to this long for the XMB to appear before showing the "loaded" notification anyway. */
#define XMB_WAIT_SEC      30

#define NID_VSHTASK_NOTIFY 0xA02D46E7  /* vshtask: (int, const char *msg) */
#define NID_PAF_VIEW_FIND  0xF21655F3  /* paf View_Find: (const char *plugin) -> view or 0 */

typedef int      (*vshtask_notify_fn)(int unk, const char *msg);
typedef uint32_t (*view_find_fn)(const char *plugin);

static vshtask_notify_fn g_vshtask_notify;
static view_find_fn      g_view_find;

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
	rm_logf("vsh exports: vshtask_notify %p, View_Find %p", g_vshtask_notify, g_view_find);
}

static bool xmb_ready(void)
{
	return g_view_find && g_view_find("explore_plugin") != 0;
}

/* Shows an XMB notification and logs it. */
static void notify(const char *fmt, ...)
{
	char msg[RIFF_NOTIFY_MAX];
	rm_buf_t b = { msg, 0, sizeof(msg) - 1 };
	va_list ap;
	va_start(ap, fmt);
	rm_vformat(&b, fmt, ap);
	va_end(ap);
	msg[b.n] = '\0';

	rm_logf("notify: '%s'%s", msg, g_vshtask_notify ? "" : "  <- vshtask_notify not found, not shown");
	if (g_vshtask_notify)
		g_vshtask_notify(0, msg);
}

/* Shows a message the game plugin posted with rm_notify_post(), if there is one. */
static void relay_game_notification(void)
{
	char msg[RIFF_NOTIFY_MAX + 1];
	int n = rm_read_file(RIFF_NOTIFY_PATH, msg, RIFF_NOTIFY_MAX);
	if (n < 0) return;
	rm_fs_unlink(RIFF_NOTIFY_PATH);
	msg[n] = '\0';
	if (n > 0) notify("%s", msg);
}

/* ------------------------------------------------------------------------ */
/* Game detection and injection                                              */
/* ------------------------------------------------------------------------ */

/*
 * debug_stage in riffmaster.cfg turns the loader's work on one step at a time,
 * to find which step freezes the console. Each stage adds to the one before.
 */
enum {
	STAGE_IDLE   = 0,  /* log and heartbeat only: no PS3MAPI calls, no notifications */
	STAGE_PIDS   = 1,  /* + XMB notifications, poll the process ID list */
	STAGE_NAMES  = 2,  /* + look up new processes' names (game detection), but don't inject */
	STAGE_INJECT = 3,  /* + inject the game plugin (normal operation, the default) */
};

static int  g_stage = STAGE_INJECT;
/* trace=1 in riffmaster.cfg logs before and after every PS3MAPI call. */
static bool g_trace;

static void load_cfg(void)
{
	static char buf[4096];
	int n = rm_read_file(RIFF_CFG_PATH, buf, sizeof(buf));
	if (n > 0) {
		g_stage = rm_cfg_int(buf, n, "debug_stage", STAGE_INJECT);
		g_trace = rm_cfg_int(buf, n, "trace", 0) != 0;
	}
	rm_logf("%s: read %d bytes, debug_stage %d%s, trace %d", RIFF_CFG_PATH, n, g_stage,
	        g_stage >= STAGE_INJECT ? " (normal)" : "", g_trace);
}

static bool is_game_process(const char *name)
{
	/* Game processes are named like "01000300_main_EBOOT.BIN". */
	return rm_memifind(name, rm_strlen(name), "eboot.bin");
}

static int get_pids(uint32_t *pids)
{
	static uint32_t failures;
	rm_memset(pids, 0, sizeof(uint32_t) * PS3MAPI_MAX_PROCESS);
	if (g_trace) rm_logf("trace: ps3mapi_get_all_pids...");
	int r = ps3mapi_get_all_pids(pids);
	if (g_trace) rm_logf("trace: ps3mapi_get_all_pids returned 0x%x", r);
	if (r != 0 && rm_log_every(&failures, 5, 100))
		rm_logf("ps3mapi_get_all_pids failed: %d (0x%x), failure #%u", r, r, failures);
	return r;
}

static bool pid_alive(uint32_t pid)
{
	uint32_t pids[PS3MAPI_MAX_PROCESS];
	if (get_pids(pids) != 0) return false;
	for (int i = 0; i < PS3MAPI_MAX_PROCESS; i++)
		if (pids[i] == pid) return true;
	return false;
}

/*
 * Processes seen in the last poll. A name is looked up once per process, and
 * only after it has been in the list for NAME_AFTER_POLLS polls in a row, so
 * PS3MAPI never reads a process that is still being created.
 */
#define NAME_AFTER_POLLS 2

typedef struct {
	uint32_t pid;
	uint32_t polls_seen;
	bool     named;
	bool     is_game;
	char     name[64];
} proc_t;

static proc_t g_procs[PS3MAPI_MAX_PROCESS];

static proc_t *find_proc(uint32_t pid)
{
	for (int i = 0; i < PS3MAPI_MAX_PROCESS; i++)
		if (g_procs[i].pid == pid) return &g_procs[i];
	return NULL;
}

static bool pid_listed(const uint32_t *pids, uint32_t pid)
{
	for (int i = 0; i < PS3MAPI_MAX_PROCESS; i++)
		if (pids[i] == pid) return true;
	return false;
}

static void lookup_name(proc_t *p)
{
	static char name[512];
	static uint32_t failures;
	rm_memset(name, 0, sizeof(name));
	if (g_trace) rm_logf("trace: ps3mapi_get_proc_name(pid 0x%08x)...", p->pid);
	int r = ps3mapi_get_proc_name(p->pid, name);
	if (g_trace) rm_logf("trace: ps3mapi_get_proc_name returned 0x%x", r);
	if (r != 0) {
		/* Retried next poll. */
		if (rm_log_every(&failures, 10, 100))
			rm_logf("ps3mapi_get_proc_name(pid 0x%08x) failed 0x%x, failure #%u", p->pid, r, failures);
		return;
	}
	size_t len = rm_strlen(name);
	if (len >= sizeof(p->name)) len = sizeof(p->name) - 1;
	rm_memcpy(p->name, name, len);
	p->name[len] = '\0';
	p->named = true;
	p->is_game = is_game_process(name);
	rm_logf("process pid 0x%08x is '%s'%s", p->pid, p->name, p->is_game ? "  <- game" : "");
}

/* Updates g_procs from a fresh pid list and logs processes that come and go. */
static void update_procs(const uint32_t *pids)
{
	for (int i = 0; i < PS3MAPI_MAX_PROCESS; i++) {
		proc_t *p = &g_procs[i];
		if (p->pid && !pid_listed(pids, p->pid)) {
			rm_logf("process gone: pid 0x%08x '%s'", p->pid, p->named ? p->name : "(name not read)");
			rm_memset(p, 0, sizeof(*p));
		}
	}
	for (int i = 0; i < PS3MAPI_MAX_PROCESS; i++) {
		if (pids[i] == 0) continue;
		proc_t *p = find_proc(pids[i]);
		if (p == NULL) {
			p = find_proc(0);
			if (p == NULL) continue;  /* can't happen: both lists hold PS3MAPI_MAX_PROCESS */
			p->pid = pids[i];
			rm_logf("new process: pid 0x%08x%s", p->pid,
			        g_stage >= STAGE_NAMES ? ", reading its name once it has settled" : "");
		}
		p->polls_seen++;
		if (g_stage >= STAGE_NAMES && !p->named && p->polls_seen >= NAME_AFTER_POLLS)
			lookup_name(p);
	}
}

static uint32_t find_game_pid(void)
{
	for (int i = 0; i < PS3MAPI_MAX_PROCESS; i++)
		if (g_procs[i].pid && g_procs[i].is_game) return g_procs[i].pid;
	return 0;
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

static void inject(uint32_t pid)
{
	log_game_plugin_file();
	rm_logf("calling ps3mapi_load_proc_module(pid 0x%08x, %s)...", pid, GAME_PLUGIN_PATH);
	int r = ps3mapi_load_proc_module(pid, GAME_PLUGIN_PATH);
	rm_logf("ps3mapi_load_proc_module returned %d (0x%x)%s", r, r, r == 0 ? "" : "  <- FAILED");
	if (r != 0)
		notify("Riffmaster: failed to load guitar plugin (0x%x)", r);
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
	if (g_stage >= STAGE_PIDS)
		resolve_vsh_exports();
	/* A message left over from an earlier game would be shown as if it were new. */
	rm_fs_unlink(RIFF_NOTIFY_PATH);
	announce_deadline = uptime_sec() + XMB_WAIT_SEC;

	while (g_running) {
		uint32_t pid = 0;
		if (g_stage >= STAGE_PIDS) {
			/* Notifications sent before the XMB is up are lost, so wait for it (or give up waiting). */
			if (announce_pending && (xmb_ready() || uptime_sec() >= announce_deadline)) {
				if (g_stage >= STAGE_INJECT)
					notify("Riffmaster loader loaded");
				else
					notify("Riffmaster loader loaded (debug stage %d)", g_stage);
				announce_pending = false;
			}
			relay_game_notification();

			if (g_trace) rm_logf("trace: poll #%u", polls);
			uint32_t pids[PS3MAPI_MAX_PROCESS];
			if (get_pids(pids) == 0)
				update_procs(pids);
			pid = find_game_pid();
		}

		if (pid && pid != handled_pid) {
			handled_pid = pid;
			if (g_stage < STAGE_INJECT) {
				rm_logf("game process found: pid 0x%08x, not injecting (debug_stage %d)", pid, g_stage);
				notify("Riffmaster: game found (debug stage %d, not loading the plugin)", g_stage);
			} else {
				rm_logf("game process found: pid 0x%08x, waiting %ds before injecting", pid, INJECT_DELAY_SEC);
				notify("Riffmaster: game found, loading guitar plugin in %ds", INJECT_DELAY_SEC);
				sleep_while_running(INJECT_DELAY_SEC);
				if (!g_running) {
					rm_logf("stop requested during inject delay");
					break;
				}
				if (pid_alive(pid))
					inject(pid);
				else
					rm_logf("pid 0x%08x exited before injection, skipping", pid);
			}
			watch_until = uptime_sec() + WATCH_SECONDS;
			rm_logf("heartbeat every 1s for the next %ds", WATCH_SECONDS);
		} else if (!pid && handled_pid) {
			rm_logf("game pid 0x%08x is gone, waiting for the next game", handled_pid);
			handled_pid = 0;
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
