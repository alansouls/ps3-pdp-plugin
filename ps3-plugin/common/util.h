/*
 * libc-free helpers shared by both plugins. Neither plugin can rely on libc
 * being loaded in the process it runs in, so the few routines needed live here.
 * Build with -fno-builtin -fno-tree-loop-distribute-patterns so GCC doesn't
 * turn these loops back into memset/memcpy calls.
 */
#ifndef RIFF_UTIL_H
#define RIFF_UTIL_H

#include <stdint.h>
#include <stddef.h>
#include <cell/cell_fs.h>

#define RIFF_LOG_PATH "/dev_hdd0/tmp/riffmaster.log"

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

/* Appends "<msg>\n", or "<msg> 0x<value>\n" when has_value is set. */
static inline void rm_log_impl(const char *msg, uint32_t value, int has_value)
{
	char line[128];
	size_t len = 0;
	while (msg[len] && len < sizeof(line) - 14) { line[len] = msg[len]; len++; }
	if (has_value) {
		static const char hex[] = "0123456789abcdef";
		line[len++] = ' '; line[len++] = '0'; line[len++] = 'x';
		for (int shift = 28; shift >= 0; shift -= 4)
			line[len++] = hex[(value >> shift) & 0xF];
	}
	line[len++] = '\n';

	int fd;
	if (cellFsOpen(RIFF_LOG_PATH, CELL_FS_O_WRONLY | CELL_FS_O_CREAT | CELL_FS_O_APPEND,
	               &fd, NULL, 0) != CELL_FS_SUCCEEDED)
		return;
	uint64_t written;
	cellFsWrite(fd, line, len, &written);
	cellFsClose(fd);
}

#define rm_log(msg)     rm_log_impl((msg), 0, 0)
#define rm_logx(msg, v) rm_log_impl((msg), (uint32_t)(v), 1)

/* Reads up to size bytes of a file; returns bytes read or -1. */
static inline int rm_read_file(const char *path, void *buf, uint32_t size)
{
	int fd;
	uint64_t nread = 0;
	if (cellFsOpen(path, CELL_FS_O_RDONLY, &fd, NULL, 0) != CELL_FS_SUCCEEDED)
		return -1;
	cellFsRead(fd, buf, size, &nread);
	cellFsClose(fd);
	return (int)nread;
}

#endif
