/* Tiny freestanding string/mem helpers used internally by our own loader
 * and protocol code (separate from the memcpy/memset/memmove *stubs* we
 * expose to the proprietary module in runtime.c, even though they share
 * the same implementation underneath). */
#ifndef HX_STRING_H
#define HX_STRING_H

#include <stddef.h>

void *hx_memcpy(void *dst, const void *src, size_t n);
void *hx_memset(void *dst, int c, size_t n);
void *hx_memmove(void *dst, const void *src, size_t n);
int hx_streq(const char *a, const char *b);
size_t hx_strlen(const char *s);

#endif
