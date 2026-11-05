#include <string.h>

#include "scr_frame.h"

/* Nibble lookup tables for the reflected CRC-32 polynomial 0xEDB88320. */
static const uint32_t s_crc32_nibble[16] = {
    0x00000000, 0x1db71064, 0x3b6e20c8, 0x26d930ac, 0x76dc4190, 0x6b6b51f4, 0x4db26158, 0x5005713c,
    0xedb88320, 0xf00f9344, 0xd6d6a3e8, 0xcb61b38c, 0x9b64c2b0, 0x86d3d2d4, 0xa00ae278, 0xbdbdf21c,
};

uint32_t scr_crc32(uint32_t seed, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = ~seed;

    while (len--) {
        crc ^= *p++;
        crc = (crc >> 4) ^ s_crc32_nibble[crc & 0x0f];
        crc = (crc >> 4) ^ s_crc32_nibble[crc & 0x0f];
    }
    return ~crc;
}

static inline uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static inline uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

bool scr_hdr_magic_ok(const uint8_t raw[SCR_MAGIC_LEN])
{
    return raw[0] == 'S' && raw[1] == 'C' && raw[2] == 'R' && raw[3] == 'F';
}

bool scr_hdr_parse(const uint8_t raw[SCR_HEADER_SIZE], scr_hdr_t *out)
{
    if (!scr_hdr_magic_ok(raw)) {
        return false;
    }

    scr_hdr_t hdr = {
        .version = raw[4],
        .flags = raw[5],
        .seq = rd32(&raw[6]),
        .width = rd16(&raw[10]),
        .height = rd16(&raw[12]),
        .payload_len = rd32(&raw[14]),
        .crc32 = rd32(&raw[18]),
    };

    if (hdr.version != SCR_VERSION) {
        return false;
    }
    if (hdr.flags & ~SCR_FLAGS_MASK) {
        return false;
    }
    if (hdr.width == 0 || hdr.height == 0 || hdr.width > SCR_MAX_DIMENSION || hdr.height > SCR_MAX_DIMENSION) {
        return false;
    }
    if (hdr.payload_len == 0 || hdr.payload_len > SCR_MAX_PAYLOAD) {
        return false;
    }
    *out = hdr;
    return true;
}
