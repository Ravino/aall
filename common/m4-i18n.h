/* common/m4-i18n.h -- the whole interface-language mechanism: one global,
 * one macro, no gettext/.mo/.po, no new build dependency.
 *
 * Design: every user-facing string literal in this codebase is written as
 * a pair, English then Russian, wrapped in M4_T():
 *
 *   printf(M4_T("hello %s\n", <the existing Russian greeting>), name);
 *   fprintf(stderr, M4_T("%s: not found\n", <the existing Russian error>), path);
 *
 * M4_T() picks one of the two at RUNTIME, via the m4_lang global below -- it is a
 * plain ternary, not a preprocessor #if, so it works identically for a
 * literal `printf(M4_T(...))` call and for the (more common in this repo)
 * case of several adjacent string-literal fragments spread over multiple
 * source lines: each M4_T() argument can itself be such a run of adjacent
 * literals (C's normal phase-6 string concatenation still applies to each
 * argument independently, since macro argument substitution happens
 * before concatenation) -- see common/m4-conf-keys.def for the biggest
 * example (every key's comment_en/comment_ru pair).
 *
 * Caveat callers must keep in mind: gcc/clang's -Wformat DOES look through
 * a ternary of two string literals and checks BOTH branches against the
 * printf-family argument list (verified against this repo's toolchain).
 * That is a feature, not a hazard -- it is exactly what
 * catches an EN/RU pair whose printf conversions drifted out of sync --
 * but it does mean both branches of every M4_T() used as a format string
 * MUST use the exact same sequence of conversions (same types, same
 * left-to-right order; reordering with %N$ positional specifiers is legal
 * C but not used anywhere in this repo, to keep both variants boring to
 * read side by side).
 *
 * Static (file-scope) initializers CANNOT use M4_T(): `m4_lang == ...` is
 * not a constant expression, so `static const char *x = M4_T(...)` (or an
 * M4_CONF_KEY(...) table row, or any other array-of-struct literal
 * initialized at file scope) fails to compile. Those cases instead keep
 * BOTH strings as separate struct fields/macro arguments (e.g.
 * m4_conf_key_def_t.comment_en/comment_ru in m4-conf.h) and the language
 * choice is made where the field is READ, not where it is initialized.
 *
 * Language is decided ONCE, before argument parsing: daemon/src/main.c
 * calls m4_i18n_init_from_conf(m4_config()) as the very first thing in
 * main(), before looking at argv[1] (so even `momentum4 --help` and an
 * unknown-subcommand error respect it) -- env/argv cannot override it,
 * matching every other config key's precedence (util/config.h's
 * m4_config(), the one process-wide layered m4_conf_t: compiled-in <
 * @datadir@ < /etc < $XDG_CONFIG_HOME/$HOME). `momentum4d reload` (ctl.sock
 * RELOAD, daemon.c's handle_reload()) calls m4_config_reload() +
 * m4_i18n_init_from_conf() again, so a running daemon picks up a language
 * change without a restart; a one-shot CLI invocation just re-reads the
 * config fresh on every run, same as any other key.
 *
 * An unset/unreadable config, or any value other than "en"/"ru", resolves
 * to M4_LANG_EN (m4_lang's own static initializer is already 0 == EN, so
 * "config never loaded at all" -- e.g. a unit test that never calls
 * m4_i18n_init_from_conf() -- is safe by construction, not by convention;
 * the registry's own enum validation, common/m4-conf-keys.def's `language
 * = en|ru`, has already rejected an unrecognized value with a warning and
 * fallen back to the compiled default by the time any m4_conf_load_*()
 * returns, so m4_i18n_init_from_conf() itself never needs to warn).
 */
#ifndef MOMENTUM4_COMMON_M4_I18N_H
#define MOMENTUM4_COMMON_M4_I18N_H

#include "m4-conf.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
	M4_LANG_EN = 0, /* default -- see this header's own comment above */
	M4_LANG_RU = 1,
} m4_lang_t;

/* The single global every M4_T() call reads. Deliberately a plain global,
 * not a per-thread/per-context value: this project has no multi-language
 * multi-tenant use case (one user, one config, one process tree), and a
 * global keeps M4_T() usable from anywhere -- including static
 * initializer-adjacent helper functions -- without threading a context
 * pointer through every printf/log call site in ~50 files. */
extern m4_lang_t m4_lang;

/* M4_T(en, ru) -- returns `ru` if the current language is Russian,
 * `en` otherwise (i.e. also for any value m4_lang could never actually
 * hold, which cannot happen since m4_lang_parse() is the only writer and
 * only accepts the two enumerators above -- the ternary's default-to-en
 * is belt-and-suspenders, not a real third case). */
#define M4_T(en, ru) ((m4_lang == M4_LANG_RU) ? (ru) : (en))

/* Parses a [general] language config value ("en"/"ru", case-insensitive;
 * NULL or anything else is rejected). Returns true and stores into `*out`
 * on a recognized value, false (leaving *out untouched) otherwise -- the
 * caller decides what "unrecognized" means (m4_i18n_init_from_conf()
 * below: warn and keep M4_LANG_EN). */
int m4_lang_parse(const char *value, m4_lang_t *out);

/* "en" or "ru" -- the inverse of m4_lang_parse(), e.g. for `momentum4
 * config get general.language` to print back the canonical form. */
const char *m4_lang_name(m4_lang_t lang);

/* Reads [general] language out of `c` (m4_config()'s singleton, or any
 * other already-loaded m4_conf_t, e.g. a test's own) and updates m4_lang.
 * `c` may be NULL (keeps the current m4_lang, does not crash) -- callers
 * that have not loaded a config yet (or never will, e.g. most unit tests)
 * do not need a NULL check of their own. See this header's own top comment
 * for who calls this and when. */
void m4_i18n_init_from_conf(const m4_conf_t *c);

#ifdef __cplusplus
}
#endif

#endif /* MOMENTUM4_COMMON_M4_I18N_H */
