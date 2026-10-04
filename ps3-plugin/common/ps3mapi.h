/*
 * Minimal Cobra PS3MAPI (syscall 8) wrappers, matching the opcodes used by
 * webMAN MOD / Cobra 8.x.
 */
#ifndef RIFF_PS3MAPI_H
#define RIFF_PS3MAPI_H

#include <stdint.h>
#include <sys/syscall.h>

#define SYSCALL8_OPCODE_PS3MAPI             0x7777
#define PS3MAPI_OPCODE_SET_PROC_MEM         0x0032
#define PS3MAPI_OPCODE_GET_ALL_PROC_MODULE_PID 0x0041
#define PS3MAPI_OPCODE_GET_PROC_MODULE_NAME 0x0042
#define PS3MAPI_OPCODE_LOAD_PROC_MODULE     0x0044

/* Cobra always copies this many module IDs into the list. */
#define PS3MAPI_MAX_MODULES 128

/*
 * Don't list processes through PS3MAPI (GET_ALL_PROC_PID / GET_PROC_NAME_BY_PID)
 * to detect games: GET_ALL_PROC_PID hung the whole console when called while a
 * game was starting. The loader asks VSH for the game's pid instead.
 */

static inline int ps3mapi_set_proc_mem(uint32_t pid, uint32_t addr, const void *buf, uint32_t size)
{
	system_call_6(8, SYSCALL8_OPCODE_PS3MAPI, PS3MAPI_OPCODE_SET_PROC_MEM,
	              (uint64_t)pid, (uint64_t)addr, (uint64_t)(uint32_t)buf, (uint64_t)size);
	return_to_user_prog(int);
}

/* Fills ids[PS3MAPI_MAX_MODULES] with the process's module IDs, 0 after the last one. */
static inline int ps3mapi_get_proc_modules(uint32_t pid, int32_t *ids)
{
	system_call_4(8, SYSCALL8_OPCODE_PS3MAPI, PS3MAPI_OPCODE_GET_ALL_PROC_MODULE_PID,
	              (uint64_t)pid, (uint64_t)(uint32_t)ids);
	return_to_user_prog(int);
}

/* Copies the module's name (up to 29 chars, NOT NUL-terminated) into name. */
static inline int ps3mapi_get_proc_module_name(uint32_t pid, int32_t id, char *name)
{
	system_call_5(8, SYSCALL8_OPCODE_PS3MAPI, PS3MAPI_OPCODE_GET_PROC_MODULE_NAME,
	              (uint64_t)pid, (uint64_t)(uint32_t)id, (uint64_t)(uint32_t)name);
	return_to_user_prog(int);
}

/* Cobra copies arg (up to 64 KB) into the process and passes it to module_start as argp. */
static inline int ps3mapi_load_proc_module(uint32_t pid, const char *path, const void *arg, uint32_t arg_size)
{
	system_call_6(8, SYSCALL8_OPCODE_PS3MAPI, PS3MAPI_OPCODE_LOAD_PROC_MODULE,
	              (uint64_t)pid, (uint64_t)(uint32_t)path, (uint64_t)(uint32_t)arg, (uint64_t)arg_size);
	return_to_user_prog(int);
}

#endif
