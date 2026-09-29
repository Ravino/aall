/*
 * aptX Adaptive OTA transport header parser.
 *
 * Adapted (freestanding port: no <errno.h>, plain int status codes instead
 * of -EINVAL/-EAGAIN) from src/aptx-adaptive-stream.c in
 * github.com/arkq/openaptx pull request #12 (Arkadiusz Bokowy and
 * contributors, MIT License). Only the
 * pure-parsing logic (payload-size table, channel-mode table, header
 * layout) is reused; nothing here touches the proprietary CAPI module.
 *
 * Whether our module's process() output actually carries this 8-byte
 * prefix is UNRESOLVED -- the
 * host-side bench tool tries to parse it and falls back to treating the
 * whole output as a raw frame if it doesn't fit.
 */
#ifndef APTX_ADAPTIVE_OTA_H
#define APTX_ADAPTIVE_OTA_H

#include <stddef.h>
#include <stdint.h>

#define OTA_HEADER_SIZE 8
#define OTA_OK 0
#define OTA_EINVAL 1
#define OTA_EAGAIN 2

enum ota_version { OTA_R2 = 2, OTA_R3 = 3, OTA_R2_2 = 22 };

struct ota_header {
	uint16_t ttp;
	uint8_t period;
	uint8_t packet_type;
	uint8_t channel_mode;
	uint32_t session_id;
	size_t payload_size;
	uint8_t decoder_channel_mode;
	enum ota_version version;
};

int ota_parse_header(const uint8_t data[OTA_HEADER_SIZE], struct ota_header *header);
int ota_next_packet(const uint8_t *data, size_t size, struct ota_header *header,
		const uint8_t **payload, size_t *consumed);

#endif
