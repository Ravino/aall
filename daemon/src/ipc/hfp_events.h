/* ipc/hfp_events.h -- receives HFP call-control events forwarded by patch
 * 0006 in our private libspa-bluez5 (module/patches/0006-...,
 * m4-src/m4-hfp-events.c), used to drive gestures and earcon timing.
 *
 * Background: MOMENTUM 4, in HFP mode, sends its own gestures (swipe back =
 * "end call" per the manual, etc.) as plain HFP AT commands over RFCOMM
 * (AT+CHUP, ATA, AT+BLDN, and -- if we ever advertise VOICE_RECOG --
 * AT+BVRA). PipeWire's spa/plugins/bluez5/backend-native.c is the only code
 * that ever sees these bytes (BlueZ hands the whole RFCOMM channel to
 * PipeWire via Profile1.NewConnection); upstream, with no ModemManager
 * registered (this machine never has one), it just answers +CME ERROR and
 * does nothing else. Patch 0006 answers OK instead for exactly these four
 * commands and fans the event out over a best-effort UNIX datagram socket
 * -- this module is
 * the receiving end, living in momentum4d.
 *
 * Design mirrors ipc/ctl.c (same runtime_dir, same "unlink stale file then
 * bind" idiom) and control/mpris_player.h's m4_player_event (verb + sender
 * info + both clock readings) -- gesture events and HFP events are
 * deliberately kept as two separate small modules/event types rather than
 * unified into one, because their transports, security models (D-Bus
 * sender identity vs. SO_PASSCRED) and wire formats are different enough
 * that a shared abstraction would only obscure both; daemon.c's
 * m4_daemon_hfp_event() applies the exact same call-action machinery
 * (daemon_gesture_call()-style cooldown/gating) that
 * m4_daemon_player_event() already does for gestures.
 */
#ifndef MOMENTUM4_IPC_HFP_EVENTS_H
#define MOMENTUM4_IPC_HFP_EVENTS_H

#include <stdint.h>

#include <systemd/sd-event.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
	M4_HFP_VERB_CHUP = 0,
	M4_HFP_VERB_ATA,
	M4_HFP_VERB_BLDN,
	M4_HFP_VERB_BVRA,
	/* patch 0007 ("tap = mic" task, diagnostic-only):
	 * ANY other AT command backend-native.c could not name -- see
	 * m4_hfp_events_notify_raw() in the SPA plugin. Carries `raw_text`
	 * below; does NOT map to any hfp-chup/ata/bldn/bvra settings action
	 * (daemon.c's m4_daemon_hfp_event() only logs it -- we do not know
	 * this command's semantics well enough to act on it automatically). */
	M4_HFP_VERB_RAW,
	/* patch 0008: the HFP audio link (eSCO) of M4.
	 * Same 3-field wire format as the named verbs. SCODROP: M4 closed an
	 * in-use link (double tap); SCOUP: patch 0008 reconnected it;
	 * SCOFAIL: patch 0008 gave up (or restore is disabled). */
	M4_HFP_VERB_SCODROP,
	M4_HFP_VERB_SCOUP,
	M4_HFP_VERB_SCOFAIL,
	M4_HFP_VERB_COUNT,
} m4_hfp_verb_t;

/* "CHUP", "ATA", "BLDN", "BVRA", "RAW", "SCODROP", "SCOUP", "SCOFAIL" */
const char *m4_hfp_verb_name(m4_hfp_verb_t v);

/* Inverse of m4_hfp_verb_name() (exact, case-sensitive); 0 or -1. */
int m4_hfp_verb_from_name(const char *name, m4_hfp_verb_t *out);

/* true for SCODROP/SCOUP/SCOFAIL (patch 0008 link events, not AT commands) */
static inline int m4_hfp_verb_is_sco(m4_hfp_verb_t v)
{
	return v == M4_HFP_VERB_SCODROP || v == M4_HFP_VERB_SCOUP || v == M4_HFP_VERB_SCOFAIL;
}

struct m4_hfp_event {
	m4_hfp_verb_t verb;
	char bdaddr[32];               /* as sent by the SPA plugin, e.g.
					 * "00:1B:66:00:00:01", or the
					 * "00:00:00:00:00:00" placeholder if
					 * it did not know the device address */
	/* Only meaningful (non-empty) when verb == M4_HFP_VERB_RAW: the
	 * sanitised/truncated AT command text (m4-hfp-events.c's
	 * M4_HFP_EVENTS_RAW_MAX_TOKEN, 48 bytes), e.g. "AT+CLCC". Empty for
	 * every named verb. */
	char raw_text[64];
	uint64_t wire_realtime_usec;   /* CLOCK_REALTIME (ms precision) in the
					 * SPA plugin's process, i.e. inside
					 * WirePlumber -- for correlating this
					 * event with WirePlumber's OWN log only.
					 * NOT used for any timing/cooldown
					 * decision here: two different
					 * processes' clocks, even both
					 * CLOCK_REALTIME, are not safely
					 * comparable for that. */
	uint64_t realtime_usec;        /* CLOCK_REALTIME in momentum4d, at the
					 * moment THIS process received the
					 * datagram */
	double monotonic_s;            /* CLOCK_MONOTONIC seconds, same moment
					 * -- this is what daemon.c's cooldown
					 * logic actually uses, same convention
					 * as control/mpris_player.h's
					 * m4_player_event.monotonic_s */
};

/* Pure parser (no I/O -- testable standalone, tests/test_hfp_events.c):
 * parses one line of patch 0006's wire format, "<VERB> <bdaddr> <ts_ms>"
 * for the four named verbs, OR patch 0007's "RAW <token>
 * <bdaddr> <ts_ms>" for M4_HFP_VERB_RAW (ASCII, space-separated, no
 * trailing newline -- but a trailing '\n'/'\r', if present, is tolerated
 * and ignored, since sscanf's "%*[^\n]"-free "%s" conversions already stop
 * at any whitespace including '\n').
 *
 * Fills verb/bdaddr/wire_realtime_usec (from <ts_ms>, converted to
 * microseconds -- the sender only has millisecond resolution, see
 * m4-hfp-events.c) and, for M4_HFP_VERB_RAW only, raw_text. Leaves
 * realtime_usec/monotonic_s at 0: those are always filled in by the CALLER
 * with ITS OWN clock reading at actual receive time (m4_hfp_events_open()'s
 * io callback does this for the live socket path; tests fill them in
 * directly to drive synthetic timelines, same convention project-wide, see
 * e.g. control/mpris_player.h's doc comment on m4_player_event).
 *
 * Returns 0 on success, -1 if:
 *   - `line`/`out` is NULL;
 *   - the line does not have exactly 3 whitespace-separated fields (named
 *     verbs) or exactly 4 (RAW: "RAW" itself, the token, bdaddr, ts_ms);
 *   - the first field is not one of "CHUP"/"ATA"/"BLDN"/"BVRA"/"RAW"
 *     (forward compatibility: a future SPA plugin build, or a stray/
 *     malicious writer, sending anything else is dropped here, not a
 *     crash/UB);
 *   - the bdaddr field does not fit `out->bdaddr` (>31 bytes -- cannot
 *     happen with a real BlueZ address, but a hostile/corrupt sender must
 *     not overflow anything: %31s already bounds the read itself, this
 *     is belt-and-suspenders, see the .c);
 *   - the timestamp field is not a valid non-negative integer.
 * Every failure is a silent "drop this datagram", never partial output:
 * `*out` is fully zeroed before any field is filled in, and is left
 * zeroed (not partially filled) on any -1 return. */
int m4_hfp_event_parse(const char *line, struct m4_hfp_event *out);

typedef void (*m4_hfp_event_cb)(void *user_data, const struct m4_hfp_event *ev);

struct m4_hfp_events;

/* Binds an AF_UNIX SOCK_DGRAM socket at "<runtime_dir>/hfp-events.sock"
 * (same directory ctl.sock/daemon.lock/mode/safe-latch already live in --
 * core/state.h's runtime_dir) and registers an sd-event io source for it.
 *
 * Returns NULL on any failure (logged at WARN) -- NOT fatal for the daemon
 * as a whole, same policy as m4_micdetect_open() failing: momentum4d keeps
 * running with HFP call-control gesture events simply unavailable (the
 * journal-scraping fallback.4 variant A, is NOT
 * implemented here -- variant B is the only wired-up path
 * report for the tradeoff).
 *
 * The io callback never blocks: the socket is O_NONBLOCK, drains up to a
 * bounded number of pending datagrams per dispatch (stops at the first
 * EAGAIN or the bound, whichever comes first, to avoid one flooding peer
 * starving the rest of the event loop), and calls `cb` once per
 * well-formed, known-verb, same-uid datagram. Malformed lines
 * (m4_hfp_event_parse() failure) and datagrams from a different uid
 * (SO_PASSCRED / SCM_CREDENTIALS check -- this socket has no other
 * authentication, unlike ctl.sock's SOCK_STREAM + accept4() + SO_PEERCRED,
 * because AF_UNIX SOCK_DGRAM has no accept() step to hang the check off of)
 * are dropped and logged at DEBUG, never delivered to `cb`. */
struct m4_hfp_events *m4_hfp_events_open(sd_event *event, const char *runtime_dir,
					   m4_hfp_event_cb cb, void *user_data);

void m4_hfp_events_close(struct m4_hfp_events *h);

#ifdef __cplusplus
}
#endif

#endif /* MOMENTUM4_IPC_HFP_EVENTS_H */
