#include "hx_string.h"

void *hx_memcpy(void *dst, const void *src, size_t n)
{
	unsigned char *d = (unsigned char *)dst;
	const unsigned char *s = (const unsigned char *)src;
	while (n--)
		*d++ = *s++;
	return dst;
}

void *hx_memset(void *dst, int c, size_t n)
{
	unsigned char *d = (unsigned char *)dst;
	while (n--)
		*d++ = (unsigned char)c;
	return dst;
}

void *hx_memmove(void *dst, const void *src, size_t n)
{
	unsigned char *d = (unsigned char *)dst;
	const unsigned char *s = (const unsigned char *)src;

	if (d == s || n == 0)
		return dst;
	if (d < s) {
		while (n--)
			*d++ = *s++;
	} else {
		d += n;
		s += n;
		while (n--)
			*--d = *--s;
	}
	return dst;
}

int hx_streq(const char *a, const char *b)
{
	while (*a && *a == *b) {
		a++;
		b++;
	}
	return *a == *b;
}

size_t hx_strlen(const char *s)
{
	size_t n = 0;
	while (s[n])
		n++;
	return n;
}
