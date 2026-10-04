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
#define PS3MAPI_OPCODE_LOAD_PROC_MODULE     0x0044

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

static inline int ps3mapi_load_proc_module(uint32_t pid, const char *path)
{
	system_call_6(8, SYSCALL8_OPCODE_PS3MAPI, PS3MAPI_OPCODE_LOAD_PROC_MODULE,
	              (uint64_t)pid, (uint64_t)(uint32_t)path, 0, 0);
	return_to_user_prog(int);
}

#endif
