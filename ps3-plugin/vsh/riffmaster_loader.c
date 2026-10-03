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

#include "../common/util.h"
#include "../common/ps3mapi.h"

SYS_MODULE_INFO(riffmaster_loader, 0, 1, 0);
SYS_MODULE_START(loader_start);
SYS_MODULE_STOP(loader_stop);

#define GAME_PLUGIN_PATH  "/dev_hdd0/plugins/riffmaster_game.sprx"
#define POLL_SECONDS      2
/* Give the game time to call cellPadInit before the plugin registers its pad. */
#define INJECT_DELAY_SEC  8

static volatile bool g_running;
static sys_ppu_thread_t g_thread;

static bool is_game_process(const char *name)
{
	/* Game processes are named like "01000300_main_EBOOT.BIN". */
	return rm_memifind(name, rm_strlen(name), "eboot.bin");
}

static bool pid_alive(uint32_t pid)
{
	uint32_t pids[PS3MAPI_MAX_PROCESS];
	rm_memset(pids, 0, sizeof(pids));
	if (ps3mapi_get_all_pids(pids) != 0) return false;
	for (int i = 0; i < PS3MAPI_MAX_PROCESS; i++)
		if (pids[i] == pid) return true;
	return false;
}

static uint32_t find_game_pid(void)
{
	uint32_t pids[PS3MAPI_MAX_PROCESS];
	static char name[512];

	rm_memset(pids, 0, sizeof(pids));
	if (ps3mapi_get_all_pids(pids) != 0) return 0;
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

static void loader_thread(uint64_t arg)
{
	(void)arg;
	uint32_t injected_pid = 0;

	rm_log("riffmaster_loader started");
	while (g_running) {
		uint32_t pid = find_game_pid();
		if (pid && pid != injected_pid) {
			sleep_while_running(INJECT_DELAY_SEC);
			if (!g_running) break;
			if (pid_alive(pid)) {
				int r = ps3mapi_load_proc_module(pid, GAME_PLUGIN_PATH);
				rm_logx("injected into pid", pid);
				rm_logx("load result", r);
				injected_pid = pid;
			}
		} else if (!pid) {
			injected_pid = 0;
		}
		sleep_while_running(POLL_SECONDS);
	}
	sys_ppu_thread_exit(0);
}

int loader_start(size_t args, void *argp)
{
	(void)args; (void)argp;
	g_running = true;
	sys_ppu_thread_create(&g_thread, loader_thread, 0, 3000, 0x2000,
	                      SYS_PPU_THREAD_CREATE_JOINABLE, "riffmaster_loader");
	return SYS_PRX_RESIDENT;
}

int loader_stop(size_t args, void *argp)
{
	(void)args; (void)argp;
	uint64_t exit_code;
	g_running = false;
	sys_ppu_thread_join(g_thread, &exit_code);
	return SYS_PRX_STOP_OK;
}
