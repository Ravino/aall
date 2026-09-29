/*
 * Reconstructed CAPI v2 prototypes for the Qualcomm Hexagon QDSP6 aptX
 * Adaptive encoder module (capi_v2_aptX_CLHDADV_Encoder.so, R2.1, SD865).
 *
 * This is NOT a copy of any Qualcomm header. Every field/offset below was
 * derived independently from the disassembly of the specific .so identified
 * by sha256 in data/capi-sha256.txt (the "LIVE" entry). Layout
 * confidence is noted per struct:
 *
 *  - capi_v2_t, capi_v2_vtbl_t: confirmed. .data.rel.ro vtable at 0x30048
 *    resolves (via R_HEX_RELATIVE) to process=0xec78, end=0xfedc,
 *    set_param=0xff20, get_param=0x129e8, set_properties=0xd6b0,
 *    get_properties=0x12a28, in that order -- matches the public AudioReach
 *    capi_vtbl_t order (github.com/Audioreach, BSD-3-Clause, Qualcomm),
 *    which is why field names below follow that header's naming.
 *  - capi_v2_buf_t, capi_v2_stream_data_t: confirmed by reading
 *    capi_v2_aptx_scalable_enc_process (0xec78): input[0] is dereferenced,
 *    then +16 is loaded as a capi_v2_buf_t*, then buf->+4 (actual_data_len)
 *    is read. This fixes buf_ptr at offset 16 and bufs_num at offset 20 of
 *    capi_v2_stream_data_t, and data_ptr@0/actual_data_len@4/max_data_len@8
 *    of capi_v2_buf_t.
 *  - capi_v2_proplist_t {props_num, prop_ptr}: confirmed (num_props read at
 *    +0 of both the init and get_static_properties proplist arguments, with
 *    a null check only on the pointer itself, not on props_num==0, i.e. an
 *    empty proplist -- props_num=0 -- is accepted and short-circuits the
 *    property-application loop in init()).
 *  - capi_v2_prop_t layout: NOT independently verified here (we do not
 *    exercise the props_num>0 path in this harness). Modelled on the public
 *    AudioReach capi_prop_t for forward compatibility only; likely, not used
 *    on the hot path.
 *  - ENC_INIT (0x00013324) 59-byte payload: confirmed struct shape
 *    against the kernel apr_audio-v2.h AFE_ID_APTX_
 *    ADAPTIVE_ENC_INIT layout, cross-checked against capi_v2_aptx_scalable_enc_
 *    set_param dispatch reading `mode` at payload offset 36 (bit 0x1000 =
 *    HQ, bit 0x2000 = LL). This is the same payload CAF's audio HAL sends
 *    to the DSP over AFE; we send it directly via set_param() instead.
 *  - Internal `me` state size: confirmed >= 17212 bytes. init() unconditio-
 *    nally does `memset(pif + 4, 0, 17212)` before touching anything else,
 *    so the capi_v2_t allocation must be at least 4 + 17212 bytes.
 */
#ifndef APTX_ADAPTIVE_CAPI_V2_H
#define APTX_ADAPTIVE_CAPI_V2_H

#include <stdint.h>

typedef int32_t capi_v2_err_t;

#define CAPI_V2_EOK 0
#define CAPI_V2_EFAILED 1
#define CAPI_V2_EBADPARAM 2

/* Minimum size of the memory block passed as `capi_v2_t *pif` to init().
 * Rounded up from the confirmed 17216 (4 + 17212) bytes with headroom;
 * get_static_properties() is not relied upon for sizing (see main.c). */
#define APTX_AD_MODULE_MEMORY_SIZE (32u * 1024u)

typedef struct capi_v2_t capi_v2_t;
typedef struct capi_v2_vtbl_t capi_v2_vtbl_t;

typedef struct {
	uint32_t props_num;
	void *prop_ptr; /* capi_v2_prop_t*, unused when props_num == 0 */
} capi_v2_proplist_t;

typedef struct {
	int8_t *data_ptr;
	uint32_t actual_data_len;
	uint32_t max_data_len;
} capi_v2_buf_t; /* confirmed 12 bytes, offsets 0/4/8 */

typedef struct {
	uint32_t flags;
	uint32_t _reserved_pad; /* keeps timestamp 8-byte aligned at +8 */
	uint64_t timestamp;
	capi_v2_buf_t *buf_ptr;
	uint32_t bufs_num;
} capi_v2_stream_data_t; /* confirmed 24 bytes, buf_ptr@16, bufs_num@20 */

struct capi_v2_t {
	const capi_v2_vtbl_t *vtbl_ptr;
};

struct capi_v2_vtbl_t {
	capi_v2_err_t (*process)(capi_v2_t *pif, capi_v2_stream_data_t *input[],
			capi_v2_stream_data_t *output[]);
	capi_v2_err_t (*end)(capi_v2_t *pif);
	capi_v2_err_t (*set_param)(capi_v2_t *pif, uint32_t param_id,
			const void *port_info_ptr, capi_v2_buf_t *params_ptr);
	capi_v2_err_t (*get_param)(capi_v2_t *pif, uint32_t param_id,
			const void *port_info_ptr, capi_v2_buf_t *params_ptr);
	capi_v2_err_t (*set_properties)(capi_v2_t *pif, capi_v2_proplist_t *proplist_ptr);
	capi_v2_err_t (*get_properties)(capi_v2_t *pif, capi_v2_proplist_t *proplist_ptr);
};

typedef capi_v2_err_t (*capi_v2_init_f)(capi_v2_t *pif,
		capi_v2_proplist_t *init_set_proplist);
typedef capi_v2_err_t (*capi_v2_get_static_properties_f)(
		capi_v2_proplist_t *init_set_proplist, capi_v2_proplist_t *static_proplist);

/* ---- aptX Adaptive scalable (music) encoder parameter IDs ---- */

/* AFE_ID_APTX_ADAPTIVE_ENC_INIT / CAPI_V2 param id, same numeric value used
 * end-to-end from AFE mixer down to this module's set_param(). confirmed
 * by cross-checking the kernel header value against this module's own
 * set_param dispatch. */
#define APTX_AD_PARAM_ENC_INIT 0x00013324u
#define APTX_AD_PARAM_BIT_RATE_LEVEL_MAP 0x000132E1u

#define APTX_AD_MODE_HQ 0x1000u
#define APTX_AD_MODE_LL 0x2000u

/* Payload for APTX_AD_PARAM_ENC_INIT. confirmed 59 bytes total,
 * packed layout. */
typedef struct __attribute__((packed)) {
	uint32_t sampling_freq;   /* 1=48k, 2=44.1k, 3=96k */
	uint32_t mtu;
	uint32_t channel_mode;    /* 0 joint-stereo, 1 mono, 2 dual mono, 4 TWS, 8 earbud */
	uint32_t min_sink_modeA;
	uint32_t max_sink_modeA;
	uint32_t min_sink_modeB;
	uint32_t max_sink_modeB;
	uint32_t min_sink_modeC;
	uint32_t max_sink_modeC;
	uint32_t mode;             /* 0x1000 HQ | 0x2000 LL */
	uint32_t input_mode;
	uint32_t fade_duration;
	uint8_t sink_cap[11];      /* aptxAdaptiveConfigStream */
} aptx_ad_enc_init_t;

_Static_assert(sizeof(aptx_ad_enc_init_t) == 59, "ENC_INIT payload must be 59 bytes");

#endif
