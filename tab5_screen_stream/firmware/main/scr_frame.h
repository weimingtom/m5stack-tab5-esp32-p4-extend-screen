/*
 * Frame header shared with python/frame_proto.py.
 *
 * Every frame the PC sends is one 22-byte header followed by the payload:
 *
 *   off  size  field
 *   0    4     magic "SCRF"
 *   4    1     version (1)
 *   5    1     flags, bit0 = 1 -> payload is JPEG, 0 -> raw RGB565
 *   6    4     seq (u32 LE)
 *   10   2     width (u16 LE)
 *   12   2     height (u16 LE)
 *   14   4     payload length (u32 LE)
 *   18   4     CRC-32 of the payload, zlib/IEEE (u32 LE)
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SCR_HEADER_SIZE 22
#define SCR_MAGIC_LEN   4
#define SCR_VERSION     1

#define SCR_FLAG_JPEG  0x01
#define SCR_FLAGS_MASK 0x01

/* Sanity limits: keep a corrupt length field from asking for a huge buffer. */
#define SCR_MAX_PAYLOAD (4u * 1024u * 1024u)
#define SCR_MAX_DIMENSION 4096

typedef struct {
    uint8_t version;
    uint8_t flags;
    uint32_t seq;
    uint16_t width;
    uint16_t height;
    uint32_t payload_len;
    uint32_t crc32;
} scr_hdr_t;

/**
 * @brief CRC-32 (IEEE 802.3, reflected), bit-compatible with Python's zlib.crc32().
 *
 * Pass a seed of 0 for a one-shot checksum over @p data.
 */
uint32_t scr_crc32(uint32_t seed, const void *data, size_t len);

/** @brief True when the first four bytes are the frame magic. */
bool scr_hdr_magic_ok(const uint8_t raw[SCR_MAGIC_LEN]);

/**
 * @brief Decode a header; returns false for unknown version/flags/dimensions.
 */
bool scr_hdr_parse(const uint8_t raw[SCR_HEADER_SIZE], scr_hdr_t *out);
