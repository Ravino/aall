/* See ota.h for provenance. */
#include "ota.h"

static const uint16_t payload_sizes[] = {
	348, 656, 140, 152, 560, 760, 960, 348, 980,
};
#define PAYLOAD_SIZE_COUNT (sizeof(payload_sizes) / sizeof(payload_sizes[0]))

struct channel_mode_map {
	uint8_t channel_mode;
	uint8_t decoder_channel_mode;
};

static const struct channel_mode_map channel_modes[] = {
	{ 0x00, 2 }, { 0x80, 1 }, { 0xa0, 4 }, { 0xc0, 5 },
};

static int channel_mode_decoder_id(uint8_t channel_mode, uint8_t *out)
{
	for (size_t i = 0; i < sizeof(channel_modes) / sizeof(channel_modes[0]); i++) {
		if (channel_modes[i].channel_mode == channel_mode) {
			*out = channel_modes[i].decoder_channel_mode;
			return 0;
		}
	}
	return -1;
}

int ota_parse_header(const uint8_t data[OTA_HEADER_SIZE], struct ota_header *header)
{
	if (!data || !header)
		return OTA_EINVAL;
	if (data[3] >= PAYLOAD_SIZE_COUNT)
		return OTA_EINVAL;

	enum ota_version version;
	switch (data[7]) {
	case 0xae: version = OTA_R2; break;
	case 0xad: version = OTA_R3; break;
	case 0xaf: version = OTA_R2_2; break;
	default: return OTA_EINVAL;
	}

	uint8_t decoder_channel_mode;
	if (channel_mode_decoder_id(data[4], &decoder_channel_mode) != 0)
		return OTA_EINVAL;

	header->ttp = (uint16_t)((uint16_t)data[0] | (uint16_t)data[1] << 8);
	header->period = data[2];
	header->packet_type = data[3];
	header->channel_mode = data[4];
	header->session_id = (uint32_t)data[4] | (uint32_t)data[5] << 8 |
			(uint32_t)data[6] << 16 | (uint32_t)data[7] << 24;
	header->payload_size = payload_sizes[data[3]];
	header->decoder_channel_mode = decoder_channel_mode;
	header->version = version;
	return OTA_OK;
}

int ota_next_packet(const uint8_t *data, size_t size, struct ota_header *header,
		const uint8_t **payload, size_t *consumed)
{
	if (!data || !header || !payload || !consumed)
		return OTA_EINVAL;
	if (size < OTA_HEADER_SIZE)
		return OTA_EAGAIN;

	int rv = ota_parse_header(data, header);
	if (rv != OTA_OK)
		return rv;

	size_t packet_size = OTA_HEADER_SIZE + header->payload_size;
	if (size < packet_size)
		return OTA_EAGAIN;

	*payload = data + OTA_HEADER_SIZE;
	*consumed = packet_size;
	return OTA_OK;
}
