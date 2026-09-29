/*
 * Hexagon-side driver: loads the proprietary aptX Adaptive CAPI v2 module
 * (path given as argv[1]), runs the CAPI init/set_param(ENC_INIT) sequence,
 * then serves the stdin/stdout protocol described in ../PROTOCOL.h. Runs
 * under qemu-hexagon-static; spawned by host/hx_bench.c.
 *
 * Encoder contract (confirmed by disassembly + live runs):
 *  - process() input is ONE interleaved L/R buffer of 32-bit words, 24-bit
 *    audio left-justified (Q31): the wrapper does `asr #8` on every word
 *    (0xf0c0), same as the aptX HD wrapper before aptxhdbtenc (0xc370).
 *    We receive S16LE from the host and convert with <<16.
 *  - In LL (internal profile 6) process() must be fed EXACTLY
 *    me->totalSamplesPerPacket interleaved words (state +512, e.g. 960 =
 *    480 frames = 10 ms) per call; any other amount yields empty output
 *    (0xf6c8: leftover != 0 -> re-sync encode, actual_data_len untouched).
 *    So we FIFO the host PCM and call process() in packet-sized chunks.
 *  - Output per packet: 8-byte OTA header + codec frame (348 B LL / 656 B
 *    HQ), out actual_data_len = 8 + frame. A reply may carry 0..N packets
 *    back-to-back (parse with ota_next_packet()).
 *
 * Env overrides (all optional, decimal or 0x-hex):
 *   APTX_AD_MODE      ll|hq|0x2000|0x1000   (default ll)
 *   APTX_AD_FEATURES  sink features u32      (default 0x0F000002 = MOMENTUM 4)
 *   APTX_AD_CAPEXT    cap_ext_ver byte       (default 1)
 *   APTX_AD_MTU       mtu (min(mtu,2008))    (default 895)
 *   APTX_AD_CHMODE    0 JS,1 mono,2 dual mono,4 TWS (default 0; 2/4 forbid LL)
 *   APTX_AD_SR        1=48k 2=44.1k 3=96k    (default 1)
 *   APTX_AD_FADE, APTX_AD_INPUT_MODE         (default 255, 0 like CAF)
 *   APTX_AD_TTP_MS    fixed TTP written into every OTA header, ms (0/unset =
 *                     module's own value); APTX_AD_TTP_US same in us (wins)
 *   APTX_AD_TTP_PIN=1 send the sideband messages that stop the module's own
 *                     incTtp drift (BLE scan=0, page scan=0, NUM_BT_LINKS=0),
 * so its TTP stays at the LL base (46 ms)
 *   APTX_AD_START_MUTE_PKTS  startup mute length after every ENC_INIT, in
 *                     packets (module default 20 = 200 ms LL / 380 ms HQ;
 *                     0 = no mute, the ~100 ms fade-in still runs)
 *   APTX_AD_NO_START_FADE=1  disable startup mute AND fade-in entirely
 *   APTX_HELPER_VERBOSE=1  keep module HAP_debug logging after init
 *   APTX_HELPER_STATE=1    dump selected state words after each process()
 */
#include "capi_v2.h"
#include "elfload.h"
#include "hx_string.h"
#include "hx_syscall.h"
#include "ota.h"

#include "../PROTOCOL.h"

#define FILE_BUF_MAX (1024u * 1024u)
static uint8_t g_file_buf[FILE_BUF_MAX];

static uint8_t g_pif_storage[APTX_AD_MODULE_MEMORY_SIZE] __attribute__((aligned(16)));

static uint8_t g_pcm_buf[AH_PCM_BYTES_MAX];
/* reply payload: 0..N OTA packets back to back (<= AH_FRAME_BYTES_MAX) */
static uint8_t g_reply_buf[AH_FRAME_BYTES_MAX];
/* one process() output: 8 + max packet (HQ table max 980) with headroom */
static uint8_t g_frame_buf[2048] __attribute__((aligned(8)));
/* S32 (Q31) interleaved FIFO fed to process(); AH_PCM_BYTES_MAX S16 bytes
 * become 2x as many S32 bytes, plus one packet of leftover. */
#define FIFO_WORDS (AH_PCM_BYTES_MAX / 2 + 4096)
static int32_t g_fifo[FIFO_WORDS] __attribute__((aligned(8)));
static uint32_t g_fifo_words;

/* Offsets into the module state (pif == me), confirmed by disassembly of
 * capi_v2_aptx_scalable_enc_process / setEncoderCfgVals. Read-only use. */
#define ME_TOTAL_SAMPLES_PER_PACKET 512 /* u32, interleaved words */
#define ME_OUT_PACKET_BYTES 528         /* u32, codec frame w/o OTA header */
#define ME_BITRATE 552                  /* u32, bps */
#define ME_PERIOD_MS 564                /* f32 */
#define ME_PROFILE 17032                /* u32: 3 = HQ, 6 = LL */
/* Startup mute/fade-in, armed at the end of ENC_INIT (0x12054..0x120a4),
 * run at the top of process() (0xf0e0..0xf2f8): while +17200 == 0 the input
 * block is zeroed and +17196 counts up to +17204 (= 20 packets); then
 * +17200 = 1 and a fade-in ramp runs for ~4800 stereo frames (LL: 10 pkts);
 * then +17192 is cleared. We only write these after
 * ENC_INIT, before the first process() of the new stream. */
#define ME_START_MUTE_ON 17192     /* u8: 1 = mute/fade-in sequence armed */
#define ME_START_MUTE_PKTS 17204   /* u32: mute length, packets */

/* Extra control command (guest-local until PROTOCOL.h gets it):
 * payload = u32 param_id, then raw set_param payload bytes. */
#define AH_CMD_RAW_SET_PARAM 2u
/* payload: u32 ttp_us; 0 = stop overriding (module's own calcTTPAdj value).
 * Rewrites OTA header bytes [0..1] (u16le, 8 us units) of every following
 * packet; the codec frame itself is not touched . */
#define AH_CMD_SET_TTP 3u
/* no payload: send the sideband messages of APTX_AD_TTP_PIN=1 now. */
#define AH_CMD_TTP_PIN 4u
#define APTX_AD_PARAM_BIT_RATE_LEVEL 0x000132E0u
#define APTX_AD_PARAM_SIDEBAND 0x000132EBu

/* OTA TTP field is u16 in 8 us units: max 524.28 ms */
#define TTP_UNITS_MAX 0xffffu
static uint32_t g_ttp_units; /* 0 = no override */

static uint32_t ttp_us_to_units(uint32_t us)
{
	uint32_t u = us / 8u;
	return u > TTP_UNITS_MAX ? TTP_UNITS_MAX : u;
}

extern int hx_module_log_enabled;

extern void hx_unresolved_import_trap(void);
extern void *hx_stdout_object(void);
extern void *qurt_elite_memory_malloc(unsigned int size, unsigned int heap_id);
extern void qurt_elite_memory_free(void *ptr);
extern void HAP_debug(unsigned int level, const char *file, unsigned int line,
		const char *fmt, ...);
extern void HAP_debug_v2(unsigned int level, const char *file, unsigned int line,
		const char *fmt, unsigned int r4, unsigned int r5, unsigned int a0, unsigned int a1, unsigned int a2,
		unsigned int a3, unsigned int a4, unsigned int a5);
extern void __register_frame_info_bases(const void *a, void *b, void *c, void *d);
extern void __cxa_finalize(void *d);
extern double _Sin(double x, unsigned int qoff);
extern float __hexagon_divsf3(float a, float b);
extern double __hexagon_muldf3(double a, double b);
extern double __hexagon_divdf3(double a, double b);
extern unsigned int __hexagon_udivsi3(unsigned int a, unsigned int b);
extern int __hexagon_divsi3(int a, int b);
extern unsigned int __hexagon_umodsi3(unsigned int a, unsigned int b);
extern int __hexagon_modsi3(int a, int b);
extern void *memcpy(void *dst, const void *src, hx_size_t n);
extern void *memset(void *dst, int c, hx_size_t n);
extern void *memmove(void *dst, const void *src, hx_size_t n);
extern void *__hexagon_memcpy_likely_aligned_min32bytes_mult8bytes(void *dst,
		const void *src, hx_size_t n);
extern int printf(const char *fmt, ...);
extern int fflush(void *stream);
extern int vsnprintf(char *buf, hx_size_t size, const char *fmt, __builtin_va_list ap);

static void log_str(const char *s) { hx_write_full(2, s, hx_strlen(s)); }

static void log_hex(unsigned int v)
{
	char buf[11] = "0x";
	for (int i = 7; i >= 0; i--) {
		int nib = (v >> (i * 4)) & 0xf;
		buf[2 + 7 - i] = nib < 10 ? (char)('0' + nib) : (char)('a' + nib - 10);
	}
	buf[10] = '\0';
	log_str(buf);
}

/* Debug aid (APTX_HELPER_STATE=1): selected module state words. Offsets
 * are from disassembly of process()/setEncoderCfgVals. */
static void dump_state(const uint8_t *me, const char *tag)
{
	static const unsigned offs[] = {
		264, 268, 280, 612, 616,
		512, 520, 524, 528, 532, 536, 540, 552, 564, 572,
		17032, 17048, 17052, 17072, 17084, 17088, 17092, 17100, 17104, 17116,
		17128, 17136, 17140,
	};
	log_str("STATE ");
	log_str(tag);
	for (unsigned i = 0; i < sizeof(offs) / sizeof(offs[0]); i++) {
		char t[8];
		int k = 0;
		unsigned v = offs[i];
		do { t[k++] = (char)('0' + v % 10); v /= 10; } while (v);
		char b[10];
		int n = 0;
		b[n++] = ' ';
		b[n++] = '+';
		while (k)
			b[n++] = t[--k];
		b[n++] = '=';
		b[n] = 0;
		log_str(b);
		uint32_t w;
		hx_memcpy(&w, me + offs[i], 4);
		log_hex(w);
	}
	log_str("\n");
}

static long read_whole_file(const char *path, uint8_t *buf, hx_size_t bufsize)
{
	long fd = hx_openat(path, HX_O_RDONLY, 0);
	if (fd < 0)
		return -1;
	hx_size_t total = 0;
	for (;;) {
		if (total >= bufsize) {
			hx_close((int)fd);
			return -1; /* file too large for g_file_buf */
		}
		long n = hx_read((int)fd, buf + total, bufsize - total);
		if (n < 0) {
			hx_close((int)fd);
			return -1;
		}
		if (n == 0)
			break;
		total += (hx_size_t)n;
	}
	hx_close((int)fd);
	return (long)total;
}

static int env_flag(char **envp, const char *name)
{
	if (!envp)
		return 0;
	hx_size_t nlen = hx_strlen(name);
	for (int i = 0; envp[i]; i++) {
		hx_size_t j = 0;
		while (envp[i][j] && name[j] && envp[i][j] == name[j])
			j++;
		if (j == nlen && envp[i][j] == '=')
			return envp[i][j + 1] == '1';
	}
	return 0;
}

static const char *env_get(char **envp, const char *name)
{
	if (!envp)
		return 0;
	hx_size_t nlen = hx_strlen(name);
	for (int i = 0; envp[i]; i++) {
		hx_size_t j = 0;
		while (envp[i][j] && name[j] && envp[i][j] == name[j])
			j++;
		if (j == nlen && envp[i][j] == '=')
			return envp[i] + j + 1;
	}
	return 0;
}

/* decimal or 0x-hex; also accepts "ll"/"hq" for APTX_AD_MODE */
static uint32_t env_u32(char **envp, const char *name, uint32_t def)
{
	const char *v = env_get(envp, name);
	if (!v || !*v)
		return def;
	if (v[0] == 'l' && v[1] == 'l') return APTX_AD_MODE_LL;
	if (v[0] == 'h' && v[1] == 'q') return APTX_AD_MODE_HQ;
	uint32_t r = 0, base = 10;
	if (v[0] == '0' && (v[1] == 'x' || v[1] == 'X')) { base = 16; v += 2; }
	for (; *v; v++) {
		uint32_t d;
		if (*v >= '0' && *v <= '9') d = (uint32_t)(*v - '0');
		else if (*v >= 'a' && *v <= 'f') d = (uint32_t)(*v - 'a' + 10);
		else if (*v >= 'A' && *v <= 'F') d = (uint32_t)(*v - 'A' + 10);
		else break;
		if (d >= base) break;
		r = r * base + d;
	}
	return r;
}

/* argv layout from _start_c: argv[0]=self, argv[1]=path to .so, then NULL,
 * then envp[]. We recover envp by scanning past the NULL after argv. */
static char **find_envp(char **argv)
{
	int i = 0;
	while (argv[i])
		i++;
	return &argv[i + 1];
}

/* Sideband messages (AVS_ENCDEC_PARAM_ID_SIDEBAND_DATA 0x132EB) that make the
 * module's LL TTP controller hold its base value. Payload = {u8 id, u8 len,
 * data...}; handlers at set_param 0x10ac4 (id 7), 0x12484 (id 14 sub 1).
 * Defaults after init() are pessimistic (0xcd5c..0xcdc8): BLE scan = page
 * scan = Wi-Fi = NUM_BT_LINKS = 1, which makes incTtp (0xe610) add 1 ms per
 * 500 packets up to base+25 ms. With BLE/page scan = 0, links = 0 and RSSI
 * >= -55 dBm (default 0.0) incTtp never fires and decTtp (0xe2ec) walks back
 * to the base (me+264 = 69 -> TTP 69-23 = 46 ms). */
static uint32_t g_start_mute_pkts = 0xffffffffu; /* 0xffffffff = module default */
static int g_no_start_fade;

static void apply_start_mute(void)
{
	if (g_no_start_fade) {
		g_pif_storage[ME_START_MUTE_ON] = 0;
	} else if (g_start_mute_pkts != 0xffffffffu) {
		hx_memcpy(g_pif_storage + ME_START_MUTE_PKTS, &g_start_mute_pkts, 4);
	}
}

static capi_v2_err_t ttp_pin(capi_v2_t *pif)
{
	static uint8_t sb_scan[5] = { 14, 3, 1, 0, 0 };  /* id 14 sub 1: BLE=0, page=0 */
	static uint8_t sb_links[3] = { 7, 1, 0 };        /* id 7: NUM_BT_LINKS=0 */
	capi_v2_err_t rc = CAPI_V2_EOK;
	capi_v2_buf_t b1 = { (int8_t *)sb_scan, sizeof(sb_scan), sizeof(sb_scan) };
	rc |= pif->vtbl_ptr->set_param(pif, APTX_AD_PARAM_SIDEBAND, 0, &b1);
	capi_v2_buf_t b2 = { (int8_t *)sb_links, sizeof(sb_links), sizeof(sb_links) };
	rc |= pif->vtbl_ptr->set_param(pif, APTX_AD_PARAM_SIDEBAND, 0, &b2);
	return rc;
}

int hx_main(int argc, char **argv)
{
	if (argc < 2) {
		log_str("usage: hx_adaptive_loader <capi_v2_aptX_CLHDADV_Encoder.so>\n");
		hx_exit_group(64);
	}
	char **envp = find_envp(argv);
	int verbose = env_flag(envp, "APTX_HELPER_VERBOSE");
	int dump = env_flag(envp, "APTX_HELPER_STATE");

	long fsize = read_whole_file(argv[1], g_file_buf, FILE_BUF_MAX);
	if (fsize <= 0) {
		log_str("failed to read module file: ");
		log_str(argv[1]);
		log_str("\n");
		hx_exit_group(65);
	}

	struct hx_import imports[] = {
		{ "printf", (uint32_t)(uintptr_t)printf },
		{ "memset", (uint32_t)(uintptr_t)memset },
		{ "__hexagon_udivsi3", (uint32_t)(uintptr_t)__hexagon_udivsi3 },
		{ "memmove", (uint32_t)(uintptr_t)memmove },
		{ "__hexagon_divsi3", (uint32_t)(uintptr_t)__hexagon_divsi3 },
		{ "fflush", (uint32_t)(uintptr_t)fflush },
		{ "memcpy", (uint32_t)(uintptr_t)memcpy },
		{ "HAP_debug_v2", (uint32_t)(uintptr_t)HAP_debug_v2 },
		{ "__hexagon_umodsi3", (uint32_t)(uintptr_t)__hexagon_umodsi3 },
		{ "__register_frame_info_bases", (uint32_t)(uintptr_t)__register_frame_info_bases },
		{ "__hexagon_memcpy_likely_aligned_min32bytes_mult8bytes",
				(uint32_t)(uintptr_t)__hexagon_memcpy_likely_aligned_min32bytes_mult8bytes },
		{ "qurt_elite_memory_malloc", (uint32_t)(uintptr_t)qurt_elite_memory_malloc },
		{ "qurt_elite_memory_free", (uint32_t)(uintptr_t)qurt_elite_memory_free },
		{ "vsnprintf", (uint32_t)(uintptr_t)vsnprintf },
		{ "HAP_debug", (uint32_t)(uintptr_t)HAP_debug },
		{ "__cxa_finalize", (uint32_t)(uintptr_t)__cxa_finalize },
		{ "__hexagon_modsi3", (uint32_t)(uintptr_t)__hexagon_modsi3 },
		{ "__hexagon_divsf3", (uint32_t)(uintptr_t)__hexagon_divsf3 },
		{ "__hexagon_muldf3", (uint32_t)(uintptr_t)__hexagon_muldf3 },
		{ "__hexagon_divdf3", (uint32_t)(uintptr_t)__hexagon_divdf3 },
		{ "_Sin", (uint32_t)(uintptr_t)_Sin },
		{ "_Stdout", (uint32_t)(uintptr_t)hx_stdout_object() },
		{ "*unresolved*", (uint32_t)(uintptr_t)hx_unresolved_import_trap },
		{ 0, 0 },
	};

	struct hx_loaded_module mod;
	if (hx_elf_load(g_file_buf, (uint32_t)fsize, imports, &mod) != 0) {
		log_str("hx_elf_load failed\n");
		hx_exit_group(66);
	}
	log_str("loaded module, base=");
	log_hex((uint32_t)(uintptr_t)mod.base);
	log_str(" size=");
	log_hex(mod.image_size);
	log_str("\n");

	uint32_t init_addr = hx_elf_find_symbol(&mod, "capi_v2_aptx_scalable_enc_init");
	if (init_addr == 0) {
		log_str("symbol capi_v2_aptx_scalable_enc_init not found "
				"(non-stripped .symtab expected)\n");
		hx_exit_group(67);
	}
	capi_v2_init_f init_fn = (capi_v2_init_f)(uintptr_t)init_addr;

	capi_v2_t *pif = (capi_v2_t *)g_pif_storage;
	hx_memset(g_pif_storage, 0, sizeof(g_pif_storage));

	/* Empty proplist: props_num == 0 short-circuits the property-apply
	 * loop in init() (confirmed, capi_v2.h header comment); init() still
	 * sets pif->vtbl_ptr and all internal defaults unconditionally. */
	capi_v2_proplist_t empty_props = { 0, 0 };
	capi_v2_err_t rc = init_fn(pif, &empty_props);
	log_str("init() -> ");
	log_hex((uint32_t)rc);
	log_str("\n");
	if (pif->vtbl_ptr == 0) {
		log_str("FATAL: init() did not set vtbl_ptr, module unusable\n");
		hx_exit_group(68);
	}

	/* ENC_INIT (0x13324), filled the way CAF does it for a 48 kHz stereo
	 * sink (a2dp_encoding.cc: sinkBuffering {20,50}x3, inputMode 0,
	 * inputFadeDuration 0xff, aptxAdaptiveConfigStream from the CIE).
	 * sink_cap[1] (features byte 0) drives the LL table selection
	 * (setEncoderCfgVals @0xdc74, ENC_INIT @0x11c24..0x11c70):
	 *   bit1 (0x02) -> btSinkCapability = 2 (R2.1), else 1 (R2.0)
	 *   bit0 (0x01) -> "TWM mode" -> LL block 3: 214 kbps / 13 ms
	 *   neither/bit1 only -> LL block 1: 270 kbps / 10 ms (levels 3..7),
	 *                        220 kbps / 12 ms (level 2)
	 * MOMENTUM 4 advertises 0x0F000002, CAF negotiates the low byte
	 * as ((snk|src)>>4)<<4 | (snk&src&0xF) = 0x02 with an R2.1 source. */
	aptx_ad_enc_init_t enc_init;
	hx_memset(&enc_init, 0, sizeof(enc_init));
	enc_init.sampling_freq = env_u32(envp, "APTX_AD_SR", 1); /* 1 = 48 kHz */
	enc_init.mtu = env_u32(envp, "APTX_AD_MTU", 895);
	enc_init.channel_mode = env_u32(envp, "APTX_AD_CHMODE", 0);
	enc_init.min_sink_modeA = 20; enc_init.max_sink_modeA = 50;
	enc_init.min_sink_modeB = 20; enc_init.max_sink_modeB = 50;
	enc_init.min_sink_modeC = 20; enc_init.max_sink_modeC = 50;
	enc_init.mode = env_u32(envp, "APTX_AD_MODE", APTX_AD_MODE_LL);
	enc_init.input_mode = env_u32(envp, "APTX_AD_INPUT_MODE", 0);
	enc_init.fade_duration = env_u32(envp, "APTX_AD_FADE", 255);
	{
		uint32_t feat = env_u32(envp, "APTX_AD_FEATURES", 0x0F000002u);
		uint8_t sink_cap[11] = { 1, (uint8_t)feat, (uint8_t)(feat >> 8),
			(uint8_t)(feat >> 16), (uint8_t)(feat >> 24), 2, 3, 3, 3, 0, 0xaa };
		sink_cap[0] = (uint8_t)env_u32(envp, "APTX_AD_CAPEXT", 1);
		hx_memcpy(enc_init.sink_cap, sink_cap, sizeof(sink_cap));
	}

	capi_v2_buf_t enc_init_buf = {
		.data_ptr = (int8_t *)&enc_init,
		.actual_data_len = sizeof(enc_init),
		.max_data_len = sizeof(enc_init),
	};
	rc = pif->vtbl_ptr->set_param(pif, APTX_AD_PARAM_ENC_INIT, 0, &enc_init_buf);
	log_str("set_param(ENC_INIT) -> ");
	log_hex((uint32_t)rc);
	log_str("\n");

	{
		uint32_t w;
		hx_memcpy(&w, g_pif_storage + ME_TOTAL_SAMPLES_PER_PACKET, 4);
		log_str("config: words/packet="); log_hex(w);
		hx_memcpy(&w, g_pif_storage + ME_OUT_PACKET_BYTES, 4);
		log_str(" packet_bytes="); log_hex(w);
		hx_memcpy(&w, g_pif_storage + ME_BITRATE, 4);
		log_str(" bitrate="); log_hex(w);
		hx_memcpy(&w, g_pif_storage + ME_PROFILE, 4);
		log_str(" profile="); log_hex(w);
		log_str("\n");
	}
	{
		uint32_t w;
		hx_memcpy(&w, g_pif_storage + ME_TOTAL_SAMPLES_PER_PACKET, 4);
		if (rc == CAPI_V2_EOK && w == 0)
			rc = CAPI_V2_EFAILED; /* no packet size selected: unusable */
	}
	if (env_get(envp, "APTX_AD_START_MUTE_PKTS"))
		g_start_mute_pkts = env_u32(envp, "APTX_AD_START_MUTE_PKTS", 20);
	g_no_start_fade = env_flag(envp, "APTX_AD_NO_START_FADE");
	apply_start_mute();
	if (rc == CAPI_V2_EOK && env_flag(envp, "APTX_AD_TTP_PIN")) {
		capi_v2_err_t prc = ttp_pin(pif);
		log_str("ttp pin sideband -> ");
		log_hex((uint32_t)prc);
		log_str("\n");
	}
	{
		uint32_t us = env_u32(envp, "APTX_AD_TTP_US", 0);
		if (!us)
			us = env_u32(envp, "APTX_AD_TTP_MS", 0) * 1000u;
		g_ttp_units = ttp_us_to_units(us);
	}
	if (dump)
		dump_state(g_pif_storage, "after_setparam");
	if (!verbose)
		hx_module_log_enabled = 0;

	/* Tell the host we are ready (protocol: u32 status, u32 detail). */
	{
		uint32_t reply[2] = { (uint32_t)rc, 0 };
		hx_write_full(1, reply, sizeof(reply));
	}

	uint32_t hdr[4];
	for (;;) {
		if (!hx_read_full(0, hdr, 4))
			break;
		uint32_t marker = hdr[0];

		if (marker == AH_CONTROL_MARKER) {
			if (!hx_read_full(0, hdr, 8))
				break;
			uint32_t cmd = hdr[0];
			uint32_t payload_bytes = hdr[1];
			if (payload_bytes > AH_PCM_BYTES_MAX)
				break;
			if (payload_bytes && !hx_read_full(0, g_pcm_buf, payload_bytes))
				break;
			capi_v2_err_t crc = CAPI_V2_EBADPARAM;
			if (cmd == AH_CMD_SET_BITRATE_LEVEL && payload_bytes >= 4) {
				capi_v2_buf_t b = { (int8_t *)g_pcm_buf, 4, 4 };
				crc = pif->vtbl_ptr->set_param(pif, APTX_AD_PARAM_BIT_RATE_LEVEL, 0, &b);
			} else if (cmd == AH_CMD_RAW_SET_PARAM && payload_bytes >= 4) {
				uint32_t pid;
				hx_memcpy(&pid, g_pcm_buf, 4);
				capi_v2_buf_t b = { (int8_t *)g_pcm_buf + 4, payload_bytes - 4,
					payload_bytes - 4 };
				crc = pif->vtbl_ptr->set_param(pif, pid, 0, &b);
				if (pid == APTX_AD_PARAM_ENC_INIT)
					apply_start_mute(); /* re-armed by every ENC_INIT */
			} else if (cmd == AH_CMD_SET_TTP && payload_bytes >= 4) {
				uint32_t us;
				hx_memcpy(&us, g_pcm_buf, 4);
				g_ttp_units = ttp_us_to_units(us);
				crc = CAPI_V2_EOK;
			} else if (cmd == AH_CMD_TTP_PIN) {
				crc = ttp_pin(pif);
			}
			if (dump)
				dump_state(g_pif_storage, "after_control");
			uint32_t reply[2] = { (uint32_t)crc, 0 };
			hx_write_full(1, reply, sizeof(reply));
			continue;
		}

		uint32_t pcm_bytes = marker;
		if (pcm_bytes == 0 || pcm_bytes > AH_PCM_BYTES_MAX || (pcm_bytes & 3))
			break;
		if (!hx_read_full(0, g_pcm_buf, pcm_bytes))
			break;

		/* S16LE interleaved -> S32 Q31 interleaved, appended to the FIFO */
		{
			const int16_t *pcm16 = (const int16_t *)g_pcm_buf;
			uint32_t n = pcm_bytes / 2;
			if (g_fifo_words + n > FIFO_WORDS)
				break; /* host outran us by > 1 max block: protocol misuse */
			for (uint32_t i = 0; i < n; i++)
				g_fifo[g_fifo_words + i] = (int32_t)((uint32_t)(uint16_t)pcm16[i] << 16);
			g_fifo_words += n;
		}

		capi_v2_err_t prc = CAPI_V2_EOK;
		uint32_t reply_bytes = 0;
		uint32_t consumed = 0;
		for (;;) {
			uint32_t need, pkt;
			hx_memcpy(&need, g_pif_storage + ME_TOTAL_SAMPLES_PER_PACKET, 4);
			hx_memcpy(&pkt, g_pif_storage + ME_OUT_PACKET_BYTES, 4);
			if (need == 0 || need > FIFO_WORDS / 2 || (need & 1)) {
				prc = CAPI_V2_EFAILED; /* encoder not configured */
				break;
			}
			if (g_fifo_words - consumed < need)
				break;
			if (reply_bytes + 8 + pkt > sizeof(g_reply_buf))
				break; /* keep the rest queued for the next request */

			capi_v2_buf_t in_buf = { (int8_t *)(g_fifo + consumed), need * 4, need * 4 };
			capi_v2_stream_data_t in_stream;
			hx_memset(&in_stream, 0, sizeof(in_stream));
			in_stream.buf_ptr = &in_buf;
			in_stream.bufs_num = 1;

			capi_v2_buf_t out_buf = { (int8_t *)g_frame_buf, 0, sizeof(g_frame_buf) };
			capi_v2_stream_data_t out_stream;
			hx_memset(&out_stream, 0, sizeof(out_stream));
			out_stream.buf_ptr = &out_buf;
			out_stream.bufs_num = 1;

			capi_v2_stream_data_t *in_arr[1] = { &in_stream };
			capi_v2_stream_data_t *out_arr[1] = { &out_stream };
			prc = pif->vtbl_ptr->process(pif, in_arr, out_arr);
			consumed += need;
			if (dump)
				dump_state(g_pif_storage, "after_process");
			if (prc != CAPI_V2_EOK)
				break;

			uint32_t fb = out_buf.actual_data_len;
			if (fb > sizeof(g_frame_buf) || reply_bytes + fb > sizeof(g_reply_buf)) {
				prc = CAPI_V2_EFAILED;
				break;
			}
			if (g_ttp_units && fb >= 8) {
				g_frame_buf[0] = (uint8_t)g_ttp_units;
				g_frame_buf[1] = (uint8_t)(g_ttp_units >> 8);
			}
			hx_memcpy(g_reply_buf + reply_bytes, g_frame_buf, fb);
			reply_bytes += fb;
		}
		if (consumed) {
			hx_memmove(g_fifo, g_fifo + consumed, (g_fifo_words - consumed) * 4);
			g_fifo_words -= consumed;
		}

		uint32_t reply[2] = { (uint32_t)prc, reply_bytes };
		if (hx_write_full(1, reply, sizeof(reply)) != 0)
			break;
		if (reply_bytes && hx_write_full(1, g_reply_buf, reply_bytes) != 0)
			break;
	}

	hx_exit_group(0);
	return 0;
}
