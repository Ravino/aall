/* graph/graph_host_build.c -- see graph_host_build.h. */
#include "graph_host_build.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* Small bounds-checked append helper: appends `fmt` to `*pos` within
 * `buf[0..cap)`, advancing `*pos`. Sets `*overflow` (sticky) instead of
 * writing past `cap` or leaving a truncated write in place -- every caller
 * below checks `*overflow` exactly once, at the very end, so a single
 * overflowing append anywhere aborts the whole builder cleanly (see this
 * file's header comment: "never partial/truncated output"). */
struct sbuf {
	char *buf;
	size_t cap;
	size_t pos;
	int overflow;
};

static void sbuf_init(struct sbuf *s, char *buf, size_t cap)
{
	s->buf = buf;
	s->cap = cap;
	s->pos = 0;
	s->overflow = 0;
	if (cap > 0)
		buf[0] = '\0';
}

__attribute__((format(printf, 2, 3)))
static void sbuf_appendf(struct sbuf *s, const char *fmt, ...)
{
	va_list ap;
	int n;

	if (s->overflow)
		return;
	va_start(ap, fmt);
	n = vsnprintf(s->buf + s->pos, s->cap - s->pos, fmt, ap);
	va_end(ap);
	if (n < 0 || (size_t)n >= s->cap - s->pos) {
		s->overflow = 1;
		return;
	}
	s->pos += (size_t)n;
}

const char *m4_graph_host_mac_underscore(const char *mac, char *out, size_t out_cap)
{
	size_t i;

	if (mac == NULL || mac[0] == '\0' || out == NULL)
		return NULL;
	if (strlen(mac) + 1 > out_cap)
		return NULL;
	for (i = 0; mac[i] != '\0'; i++)
		out[i] = (mac[i] == ':') ? '_' : mac[i];
	out[i] = '\0';
	return out;
}

/* Appends one `stream.rules` match entry (already inside the enclosing `[`
 * `]`) that matches `media.class = "Audio/Sink"` AND `node.name = "<value>"`
 * (value passed through as-is -- caller decides literal vs "~regex"),
 * followed by an empty `create-stream` action (same as every example in
 * module-combine-stream.c's own doc comment: no per-member remap needed,
 * M4 and its combine partners are plain stereo). */
static void append_sink_match_rule(struct sbuf *s, const char *node_name_value)
{
	sbuf_appendf(s,
		" { matches = [ { media.class = \"Audio/Sink\" node.name = \"%s\" } ] "
		"actions = { create-stream = {} } }",
		node_name_value);
}

/* Returns 1 if `s` contains a '"' (would break out of the generated JSON
 * string -- rejected by the caller), else 0. */
static int contains_quote(const char *s, size_t len)
{
	size_t i;

	for (i = 0; i < len; i++)
		if (s[i] == '"')
			return 1;
	return 0;
}

int m4_graph_host_build_combine_args(const struct m4_graph_host_config *cfg,
				      char *out, size_t out_cap)
{
	struct sbuf s;
	const char *p;
	char mac_u[32];
	int any_member = 0;

	if (cfg == NULL || out == NULL || out_cap == 0)
		return -1;

	sbuf_init(&s, out, out_cap);

	sbuf_appendf(&s, "{ combine.mode = sink node.name = \"%s\" "
			 "node.description = \"momentum4 combined output\" "
			 "combine.latency-compensate = %s stream.rules = [",
		     (cfg->combine_name != NULL && cfg->combine_name[0] != '\0')
			     ? cfg->combine_name
			     : "m4_combined",
		     cfg->combine_latency_compensate ? "true" : "false");

	/* combine_members_csv: comma-separated tokens, trimmed, empty tokens
	 * skipped, a '"' in any token fails the whole builder (see header). */
	p = cfg->combine_members_csv;
	while (p != NULL && *p != '\0') {
		const char *start;
		const char *end;
		size_t len;

		while (*p == ',' || *p == ' ' || *p == '\t')
			p++;
		if (*p == '\0')
			break;
		start = p;
		while (*p != '\0' && *p != ',')
			p++;
		end = p;
		while (end > start && (end[-1] == ' ' || end[-1] == '\t'))
			end--;
		len = (size_t)(end - start);
		if (len == 0)
			continue;
		if (contains_quote(start, len))
			return -1;
		{
			char token[256];

			if (len >= sizeof(token))
				return -1;
			memcpy(token, start, len);
			token[len] = '\0';
			append_sink_match_rule(&s, token);
			any_member = 1;
		}
	}

	if (cfg->m4_mac != NULL && cfg->m4_mac[0] != '\0' &&
	    m4_graph_host_mac_underscore(cfg->m4_mac, mac_u, sizeof(mac_u)) != NULL) {
		char m4_pattern[64];

		snprintf(m4_pattern, sizeof(m4_pattern), "bluez_output.%s.1", mac_u);
		append_sink_match_rule(&s, m4_pattern);
		any_member = 1;
	}

	sbuf_appendf(&s, " ] }");

	(void)any_member; /* an empty combine (no members yet) is still a
			    * valid, loadable module -- graph_host_run.c logs
			    * a warning in that case, this builder does not
			    * need to know about live state to decide that. */

	if (s.overflow)
		return -1;
	return (int)s.pos;
}

int m4_graph_host_build_mic_args(const struct m4_graph_host_config *cfg,
				  char *out, size_t out_cap)
{
	struct sbuf s;
	const char *mic_name;

	if (cfg == NULL || out == NULL || out_cap == 0)
		return -1;

	mic_name = (cfg->mic_name != NULL && cfg->mic_name[0] != '\0')
			   ? cfg->mic_name
			   : "momentum4_mic";

	sbuf_init(&s, out, out_cap);

	sbuf_appendf(&s, "{ audio.channels = 1 capture.props = { filter.smart = true ");

	if (cfg->m4_mac != NULL && cfg->m4_mac[0] != '\0') {
		if (contains_quote(cfg->m4_mac, strlen(cfg->m4_mac)))
			return -1;
		sbuf_appendf(&s,
			"filter.smart.target = {\"api.bluez5.address\":\"%s\",\"media.class\":\"Audio/Source\"} ",
			cfg->m4_mac);
	}

	sbuf_appendf(&s,
		"node.passive = true node.name = \"%s_capture\" stream.dont-remix = true "
		"node.dont-fallback = true state.restore-props = false } "
		"playback.props = { media.class = Audio/Source node.name = \"%s\" "
		"node.description = \"momentum4 mic placeholder\" node.virtual = false "
		"node.dont-fallback = true node.linger = true state.restore-props = false } }",
		mic_name, mic_name);

	if (s.overflow)
		return -1;
	return (int)s.pos;
}
