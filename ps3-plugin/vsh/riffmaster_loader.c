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

static bool is_game_process(const char *name)
{
	/* Game processes are named like "01000300_main_EBOOT.BIN". */
	return rm_memifind(name, rm_strlen(name), "eboot.bin");
}

static int get_pids(uint32_t *pids)
{
	static uint32_t failures;
	rm_memset(pids, 0, sizeof(uint32_t) * PS3MAPI_MAX_PROCESS);
	int r = ps3mapi_get_all_pids(pids);
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

/* Logs the full process list whenever it differs from the last one seen. */
static void log_process_list_if_changed(const uint32_t *pids)
{
	static uint32_t last[PS3MAPI_MAX_PROCESS];
	static char name[512];
	bool changed = false;
	for (int i = 0; i < PS3MAPI_MAX_PROCESS; i++)
		if (pids[i] != last[i]) changed = true;
	if (!changed) return;
	rm_memcpy(last, pids, sizeof(last));

	rm_logf("process list changed:");
	for (int i = 0; i < PS3MAPI_MAX_PROCESS; i++) {
		if (pids[i] == 0) continue;
		rm_memset(name, 0, sizeof(name));
		int r = ps3mapi_get_proc_name(pids[i], name);
		rm_logf("  [%d] pid 0x%08x name_r=%d name='%.200s'%s", i, pids[i], r, name,
		        (r == 0 && is_game_process(name)) ? "  <- game" : "");
	}
}

static uint32_t find_game_pid(void)
{
	uint32_t pids[PS3MAPI_MAX_PROCESS];
	static char name[512];

	if (get_pids(pids) != 0) return 0;
	log_process_list_if_changed(pids);
	for (int i = 0; i < PS3MAPI_MAX_PROCESS; i++) {
		if (pids[i] == 0) continue;
		rm_memset(name, 0, sizeof(name));
		if (ps3mapi_get_proc_name(pids[i], name) == 0 && is_game_process(name))
			return pids[i];
	}
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

static void loader_thread(uint64_t arg)
{
	(void)arg;
	uint32_t injected_pid = 0;
	uint64_t watch_until = 0;
	uint32_t polls = 0;

	begin_log();
	rm_logf("loader thread running, uptime %llus, poll %ds, inject delay %ds",
	        uptime_sec(), POLL_SECONDS, INJECT_DELAY_SEC);
	log_game_plugin_file();

	while (g_running) {
		uint32_t pid = find_game_pid();
		if (pid && pid != injected_pid) {
			rm_logf("game process found: pid 0x%08x, waiting %ds before injecting", pid, INJECT_DELAY_SEC);
			sleep_while_running(INJECT_DELAY_SEC);
			if (!g_running) {
				rm_logf("stop requested during inject delay");
				break;
			}
			if (pid_alive(pid)) {
				log_game_plugin_file();
				rm_logf("calling ps3mapi_load_proc_module(pid 0x%08x, %s)...", pid, GAME_PLUGIN_PATH);
				int r = ps3mapi_load_proc_module(pid, GAME_PLUGIN_PATH);
				rm_logf("ps3mapi_load_proc_module returned %d (0x%x)%s", r, r, r == 0 ? "" : "  <- FAILED");
				injected_pid = pid;
				watch_until = uptime_sec() + WATCH_SECONDS;
				rm_logf("heartbeat every 1s for the next %ds", WATCH_SECONDS);
			} else {
				rm_logf("pid 0x%08x exited before injection, skipping", pid);
			}
		} else if (!pid && injected_pid) {
			rm_logf("game pid 0x%08x is gone, waiting for the next game", injected_pid);
			injected_pid = 0;
			watch_until = 0;
		}

		polls++;
		bool watching = uptime_sec() < watch_until;
		if (watching || polls % HEARTBEAT_POLLS == 0)
			rm_logf("heartbeat: uptime %llus, game pid 0x%08x, injected pid 0x%08x",
			        uptime_sec(), pid, injected_pid);
		sleep_while_running(watching ? 1 : POLL_SECONDS);
	}
	rm_logf("loader thread exiting");
	sys_ppu_thread_exit(0);
}

int loader_start(size_t args, void *argp)
{
	(void)args; (void)argp;
	g_running = true;
	int r = sys_ppu_thread_create(&g_thread, loader_thread, 0, 3000, 0x2000,
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
