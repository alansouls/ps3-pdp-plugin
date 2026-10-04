/*
 * Progress reporting from the game plugin to the loader, without the filesystem.
 *
 * The loader passes a riff_arg_t to the game plugin's module_start (PS3MAPI
 * LOAD_PROC_MODULE copies it into the game process). The game plugin then
 * writes a riff_status_t into the loader's memory in VSH with PS3MAPI
 * SET_PROC_MEM after each setup step. The loader logs every change and shows
 * the XMB notifications, so progress is visible even if the game process
 * can't write files.
 */
#ifndef RIFF_STATUS_H
#define RIFF_STATUS_H

#include <stdint.h>

#define RIFF_ARG_MAGIC 0x52494646  /* "RIFF" */

typedef struct {
	uint32_t magic;
	uint32_t vsh_pid;      /* the loader's process */
	uint32_t status_addr;  /* riff_status_t in the loader's memory */
} riff_arg_t;

enum {
	RIFF_STEP_NONE = 0,
	RIFF_STEP_MODULE_START,   /* riff_start ran; result 0 = arg came in args, 1 = in argp, 2 = not found */
	RIFF_STEP_THREAD,         /* riff_thread is running */
	RIFF_STEP_PROFILE,        /* profile selected */
	RIFF_STEP_IMPORTS,        /* result = number of pad imports hooked */
	RIFF_STEP_USBD_MODULE,    /* result = cellSysmoduleLoadModule(USBD) */
	RIFF_STEP_USBD_INIT,      /* result = cellUsbdInit */
	RIFF_STEP_READY,          /* result = cellUsbdRegisterExtraLdd2; setup finished */
	RIFF_STEP_ATTACHED,       /* the guitar was attached */
	RIFF_STEP_FIRST_REPORT,   /* the first input report arrived */
	RIFF_STEP_COUNT
};

/*
 * The game plugin's copy starts with this signature (in initialized data), so
 * the loader can also find and read it straight from the game's memory with
 * PS3MAPI GET_PROC_MEM. That works even if module_start never got the
 * loader's argument or can't make syscalls.
 */
#define RIFF_STATUS_SIG0 0x52494646  /* "RIFF" */
#define RIFF_STATUS_SIG1 0x53544154  /* "STAT" */

typedef struct {
	uint32_t sig[2];   /* RIFF_STATUS_SIG0/1 in the game plugin's copy */
	uint32_t seq;      /* bumped on every report */
	uint32_t step;     /* RIFF_STEP_*: the last step reached */
	int32_t  result;   /* that step's return code */
	int32_t  log_err;  /* last error opening the log file in the game, 0 if logging works */
	char     profile;  /* 'g' or 'r' once the profile is selected */
	char     pad[3];
	/* For when reports don't reach the loader: */
	int32_t  report_err;  /* last ps3mapi_set_proc_mem result, 0 if it worked */
	uint32_t start_args;  /* module_start's args and argp, as received */
	uint32_t start_argp;
	uint32_t log_probe;   /* last RIFF_LOG_PROBE point reached inside rm_logf (see util.h) */
	uint32_t mark_line;   /* source line of the last mark() in the game plugin */
	int32_t  mark_result; /* result recorded with it */
} riff_status_t;

#endif
