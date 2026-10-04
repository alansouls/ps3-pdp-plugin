/*
 * libc-free helpers shared by both plugins. Neither plugin can rely on libc
 * being loaded in the process it runs in, so the few routines needed live here.
 * Build with -fno-builtin -fno-tree-loop-distribute-patterns so GCC doesn't
 * turn these loops back into memset/memcpy calls.
 *
 * Define RIFF_LOG_TAG before including this header to tag each log line.
 */
#ifndef RIFF_UTIL_H
#define RIFF_UTIL_H

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <sys/syscall.h>
#include <sys/sys_time.h>
#include <sys/ppu_thread.h>
#include <cell/cell_fs.h>

#define RIFF_LOG_PATH     "/dev_hdd0/tmp/riffmaster.log"
#define RIFF_LOG_OLD_PATH "/dev_hdd0/tmp/riffmaster.old.log"
/* When the log reaches this size it is emptied and restarted, keeping the newest lines. */
#define RIFF_LOG_MAX_BYTES (1024 * 1024)

#ifndef RIFF_LOG_TAG
#define RIFF_LOG_TAG "?"
#endif

/*
 * Called at each step inside rm_logf with a number (1 = entered ... 9 = file
 * closed). The game plugin defines it to report how far a log write got.
 */
#ifndef RIFF_LOG_PROBE
#define RIFF_LOG_PROBE(n) ((void)0)
#endif

static inline void rm_memset(void *dst, int v, size_t n)
{
	uint8_t *d = (uint8_t *)dst;
	while (n--) *d++ = (uint8_t)v;
}

static inline void rm_memcpy(void *dst, const void *src, size_t n)
{
	uint8_t *d = (uint8_t *)dst;
	const uint8_t *s = (const uint8_t *)src;
	while (n--) *d++ = *s++;
}

static inline size_t rm_strlen(const char *s)
{
	size_t n = 0;
	while (s[n]) n++;
	return n;
}

static inline char rm_lower(char c)
{
	return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

/* Case-insensitive search for needle in a buffer that may contain NULs. */
static inline int rm_memifind(const char *hay, size_t hay_len, const char *needle)
{
	size_t n = rm_strlen(needle);
	if (n == 0 || hay_len < n) return 0;
	for (size_t i = 0; i + n <= hay_len; i++) {
		size_t j = 0;
		while (j < n && rm_lower(hay[i + j]) == rm_lower(needle[j])) j++;
		if (j == n) return 1;
	}
	return 0;
}

/* ------------------------------------------------------------------------ */
/* Raw lv2 filesystem syscalls                                              */
/* ------------------------------------------------------------------------ */

/*
 * These skip libfs so logging works before it is loaded in the process
 * (e.g. from module_start inside the game), and so a hang inside libfs
 * can't take the log down with it.
 */

static inline int rm_fs_open(const char *path, int flags, int *fd)
{
	system_call_6(801, (uint64_t)(uint32_t)path, (uint64_t)flags, (uint64_t)(uint32_t)fd,
	              0666, 0, 0);
	return_to_user_prog(int);
}

static inline int rm_fs_read(int fd, void *buf, uint64_t size, uint64_t *nread)
{
	system_call_4(802, (uint64_t)fd, (uint64_t)(uint32_t)buf, size, (uint64_t)(uint32_t)nread);
	return_to_user_prog(int);
}

static inline int rm_fs_write(int fd, const void *buf, uint64_t size, uint64_t *nwritten)
{
	system_call_4(803, (uint64_t)fd, (uint64_t)(uint32_t)buf, size, (uint64_t)(uint32_t)nwritten);
	return_to_user_prog(int);
}

static inline int rm_fs_close(int fd)
{
	system_call_1(804, (uint64_t)fd);
	return_to_user_prog(int);
}

static inline int rm_fs_stat(const char *path, CellFsStat *st)
{
	system_call_2(808, (uint64_t)(uint32_t)path, (uint64_t)(uint32_t)st);
	return_to_user_prog(int);
}

static inline int rm_fs_fstat(int fd, CellFsStat *st)
{
	system_call_2(809, (uint64_t)fd, (uint64_t)(uint32_t)st);
	return_to_user_prog(int);
}

static inline int rm_fs_rename(const char *from, const char *to)
{
	system_call_2(812, (uint64_t)(uint32_t)from, (uint64_t)(uint32_t)to);
	return_to_user_prog(int);
}

static inline int rm_fs_unlink(const char *path)
{
	system_call_1(814, (uint64_t)(uint32_t)path);
	return_to_user_prog(int);
}

static inline int rm_fs_fsync(int fd)
{
	system_call_1(820, (uint64_t)fd);
	return_to_user_prog(int);
}

static inline int rm_fs_ftruncate(int fd, uint64_t size)
{
	system_call_2(832, (uint64_t)fd, size);
	return_to_user_prog(int);
}

/* Reads up to size bytes of a file; returns bytes read or -1. */
static inline int rm_read_file(const char *path, void *buf, uint32_t size)
{
	int fd;
	uint64_t nread = 0;
	if (rm_fs_open(path, CELL_FS_O_RDONLY, &fd) != 0)
		return -1;
	rm_fs_read(fd, buf, size, &nread);
	rm_fs_close(fd);
	return (int)nread;
}

#define RIFF_CFG_PATH "/dev_hdd0/plugins/riffmaster.cfg"

/*
 * Returns the decimal value of the first "key=<number>" line in a cfg buffer,
 * or def if the key is missing. Lines starting with '#' are comments.
 */
static inline int rm_cfg_int(const char *buf, int n, const char *key, int def)
{
	size_t klen = rm_strlen(key);
	for (int i = 0; i < n; ) {
		int end = i;
		while (end < n && buf[end] != '\n') end++;
		if (buf[i] != '#' && (size_t)(end - i) > klen && buf[i + klen] == '=' &&
		    rm_memifind(buf + i, klen, key)) {
			int j = i + (int)klen + 1, v = 0;
			if (j >= end || buf[j] < '0' || buf[j] > '9') return def;
			while (j < end && buf[j] >= '0' && buf[j] <= '9') v = v * 10 + (buf[j++] - '0');
			return v;
		}
		i = end + 1;
	}
	return def;
}

/* ------------------------------------------------------------------------ */
/* Formatting                                                               */
/* ------------------------------------------------------------------------ */

typedef struct {
	char  *p;
	size_t n, cap;
} rm_buf_t;

static inline void rm_putc(rm_buf_t *b, char c)
{
	if (b->n < b->cap) b->p[b->n++] = c;
}

static inline void rm_putnum(rm_buf_t *b, uint64_t v, unsigned base, int neg, int width, char pad)
{
	char tmp[24];
	int i = 0;
	do { tmp[i++] = "0123456789abcdef"[v % base]; v /= base; } while (v);
	if (neg) {
		if (pad == '0') { rm_putc(b, '-'); width--; }
		else tmp[i++] = '-';
	}
	while (width-- > i) rm_putc(b, pad);
	while (i) rm_putc(b, tmp[--i]);
}

/*
 * Small vsnprintf: %d %u %x %p %s %c %%, with optional '-', '0', width,
 * ".precision" for %s, and l/ll/z length modifiers.
 */
static inline void rm_vformat(rm_buf_t *b, const char *fmt, va_list ap)
{
	for (; *fmt; fmt++) {
		if (*fmt != '%') { rm_putc(b, *fmt); continue; }
		fmt++;
		int left = 0, width = 0, prec = -1, longs = 0;
		char pad = ' ';
		if (*fmt == '-') { left = 1; fmt++; }
		if (*fmt == '0') { pad = '0'; fmt++; }
		while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
		if (*fmt == '.') {
			prec = 0; fmt++;
			while (*fmt >= '0' && *fmt <= '9') prec = prec * 10 + (*fmt++ - '0');
		}
		while (*fmt == 'l') { longs++; fmt++; }
		if (*fmt == 'z') fmt++;

		switch (*fmt) {
		case 'd': {
			int64_t v = longs >= 2 ? va_arg(ap, long long) : (int64_t)va_arg(ap, long);
			rm_putnum(b, v < 0 ? (uint64_t)-v : (uint64_t)v, 10, v < 0, width, pad);
			break;
		}
		case 'u':
		case 'x': {
			uint64_t v = longs >= 2 ? va_arg(ap, unsigned long long) : (uint64_t)va_arg(ap, unsigned long);
			rm_putnum(b, v, *fmt == 'x' ? 16 : 10, 0, width, pad);
			break;
		}
		case 'p':
			rm_putc(b, '0'); rm_putc(b, 'x');
			rm_putnum(b, (uint32_t)va_arg(ap, void *), 16, 0, 8, '0');
			break;
		case 'c':
			rm_putc(b, (char)va_arg(ap, int));
			break;
		case 's': {
			const char *s = va_arg(ap, const char *);
			if (!s) s = "(null)";
			int len = 0;
			while (s[len] && (prec < 0 || len < prec)) len++;
			if (!left) while (width-- > len) rm_putc(b, ' ');
			for (int i = 0; i < len; i++) rm_putc(b, s[i]);
			if (left) while (width-- > len) rm_putc(b, ' ');
			break;
		}
		case '%':
			rm_putc(b, '%');
			break;
		case '\0':
			return;
		default:
			rm_putc(b, '%'); rm_putc(b, *fmt);
			break;
		}
	}
}

static inline void rm_format(rm_buf_t *b, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	rm_vformat(b, fmt, ap);
	va_end(ap);
}

/* ------------------------------------------------------------------------ */
/* Logging                                                                  */
/* ------------------------------------------------------------------------ */

/*
 * Every line is fsync'd before returning, so it survives the console being
 * powered off right after. That makes logging slow (milliseconds per line),
 * so per-frame and per-report paths must rate-limit with rm_log_every().
 */
/* Last error from opening the log file, 0 once it opens. Reported to the loader by the game plugin. */
static int rm_log_err;

static inline void rm_log_write(const char *line, size_t len)
{
	int fd;
	uint64_t written;
	RIFF_LOG_PROBE(4);
	rm_log_err = rm_fs_open(RIFF_LOG_PATH, CELL_FS_O_WRONLY | CELL_FS_O_CREAT | CELL_FS_O_APPEND, &fd);
	RIFF_LOG_PROBE(5);
	if (rm_log_err != 0)
		return;
	CellFsStat st;
	if (rm_fs_fstat(fd, &st) == 0 && st.st_size + len > RIFF_LOG_MAX_BYTES) {
		static const char wrap[] = "=== log reached size limit, older lines discarded ===\n";
		rm_fs_ftruncate(fd, 0);
		rm_fs_write(fd, wrap, sizeof(wrap) - 1, &written);
	}
	RIFF_LOG_PROBE(6);
	rm_fs_write(fd, line, len, &written);
	RIFF_LOG_PROBE(7);
	rm_fs_fsync(fd);
	RIFF_LOG_PROBE(8);
	rm_fs_close(fd);
	RIFF_LOG_PROBE(9);
}

/* Appends "[uptime] tag tid: <formatted msg>\n". */
static inline void rm_logf(const char *fmt, ...)
{
	char line[256];
	rm_buf_t b = { line, 0, sizeof(line) - 1 };

	RIFF_LOG_PROBE(1);
	uint64_t us = sys_time_get_system_time();
	sys_ppu_thread_t tid = 0;
	sys_ppu_thread_get_id(&tid);
	RIFF_LOG_PROBE(2);
	rm_format(&b, "[%5llu.%06llu] %-6s t%llx: ", us / 1000000, us % 1000000,
	          RIFF_LOG_TAG, (unsigned long long)tid);

	va_list ap;
	va_start(ap, fmt);
	rm_vformat(&b, fmt, ap);
	va_end(ap);

	line[b.n++] = '\n';
	RIFF_LOG_PROBE(3);
	rm_log_write(line, b.n);
}

/* Logs n bytes as hex, 16 per line. */
static inline void rm_log_hex(const char *label, const void *data, size_t n)
{
	const uint8_t *p = (const uint8_t *)data;
	for (size_t off = 0; off < n; off += 16) {
		char hex[16 * 3 + 1];
		rm_buf_t b = { hex, 0, sizeof(hex) - 1 };
		for (size_t i = off; i < n && i < off + 16; i++)
			rm_format(&b, "%02x ", p[i]);
		hex[b.n] = '\0';
		rm_logf("%s +%02x: %s", label, (unsigned)off, hex);
	}
}

/*
 * Rate limiter for hot paths: true for the first `first` calls, then for
 * every `every`th call after that. Not atomic; an occasional extra or
 * missing line under contention doesn't matter.
 */
static inline int rm_log_every(uint32_t *counter, uint32_t first, uint32_t every)
{
	uint32_t n = (*counter)++;
	return n < first || (every && (n - first) % every == 0);
}

/* Moves the previous boot's log to RIFF_LOG_OLD_PATH so this boot starts empty. */
static inline void rm_log_rotate(void)
{
	rm_fs_unlink(RIFF_LOG_OLD_PATH);
	rm_fs_rename(RIFF_LOG_PATH, RIFF_LOG_OLD_PATH);
}

#endif
