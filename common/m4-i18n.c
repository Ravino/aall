/* common/m4-i18n.c -- see m4-i18n.h. */
#include "m4-i18n.h"

#include <stddef.h>  /* NULL */
#include <strings.h> /* strcasecmp -- POSIX, already a m4-conf.c dependency */

/* Static initializer 0 == M4_LANG_EN on purpose -- see m4-i18n.h's own
 * header comment: any translation unit that links this file but never
 * calls a language-selection routine (most unit tests) stays English. */
m4_lang_t m4_lang = M4_LANG_EN;

int m4_lang_parse(const char *value, m4_lang_t *out)
{
	if (value == NULL)
		return 0;
	if (strcasecmp(value, "en") == 0) {
		*out = M4_LANG_EN;
		return 1;
	}
	if (strcasecmp(value, "ru") == 0) {
		*out = M4_LANG_RU;
		return 1;
	}
	return 0;
}

const char *m4_lang_name(m4_lang_t lang)
{
	return lang == M4_LANG_RU ? "ru" : "en";
}

void m4_i18n_init_from_conf(const m4_conf_t *c)
{
	m4_lang_t lang;

	if (c == NULL)
		return;
	if (m4_lang_parse(m4_conf_get_string(c, "general", "language"), &lang))
		m4_lang = lang;
	/* else: NULL/unparseable -- the registry's own M4_CONF_T_ENUM
	 * validation for this key ("en|ru") already turned any unrecognized
	 * config value into a warning + the compiled default ("en") before
	 * m4_conf_load_*() ever returned, so this branch is defense in depth,
	 * not a real case; m4_lang is left exactly as it was. */
}
