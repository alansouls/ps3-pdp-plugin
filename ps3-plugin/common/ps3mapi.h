/*
 * Minimal Cobra PS3MAPI (syscall 8) wrappers, matching the opcodes used by
 * webMAN MOD / Cobra 8.x.
 */
#ifndef RIFF_PS3MAPI_H
#define RIFF_PS3MAPI_H

#include <stdint.h>
#include <sys/syscall.h>

#define SYSCALL8_OPCODE_PS3MAPI             0x7777
#define PS3MAPI_OPCODE_GET_ALL_PROC_PID     0x0021
#define PS3MAPI_OPCODE_GET_PROC_NAME_BY_PID 0x0022
#define PS3MAPI_OPCODE_SET_PROC_MEM         0x0032
#define PS3MAPI_OPCODE_LOAD_PROC_MODULE     0x0044

#define PS3MAPI_MAX_PROCESS 16

static inline int ps3mapi_get_all_pids(uint32_t *pids)
{
	system_call_3(8, SYSCALL8_OPCODE_PS3MAPI, PS3MAPI_OPCODE_GET_ALL_PROC_PID,
	              (uint64_t)(uint32_t)pids);
	return_to_user_prog(int);
}

static inline int ps3mapi_get_proc_name(uint32_t pid, char *name)
{
	system_call_4(8, SYSCALL8_OPCODE_PS3MAPI, PS3MAPI_OPCODE_GET_PROC_NAME_BY_PID,
	              (uint64_t)pid, (uint64_t)(uint32_t)name);
	return_to_user_prog(int);
}

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
