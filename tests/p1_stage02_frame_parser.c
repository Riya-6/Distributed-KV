/*
 * Phase 1, Stage 2: frame parser (pure function, no sockets).
 * Contract under test: include/kv/protocol.h -> src/protocol.c (you write this).
 * See docs/stages/phase1-tcp-server.md, Stage 2, and docs/protocol.md for
 * where these exact byte vectors come from.
 *
 * NOTE: if you changed the wire framing from the proposed default in
 * docs/protocol.md, these byte vectors no longer apply — update them
 * (and docs/protocol.md) to match your own design before this stage
 * will mean anything.
 */

#include <string.h>

#include "kv/protocol.h"
#include "test_framework.h"

/* AB 01 00 00 00 00 00 -- PING, empty payload */
static const uint8_t PING_FRAME[] = {0xAB, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00};

/* AB 03 00 00 00 0C 00 | 00 03 'f' 'o' 'o' 00 00 00 03 'b' 'a' 'r'
 * -- SET foo=bar */
static const uint8_t SET_FOO_BAR_FRAME[] = {
    0xAB, 0x03, 0x00, 0x00, 0x00, 0x0C, 0x00,
    0x00, 0x03, 'f',  'o',  'o',
    0x00, 0x00, 0x00, 0x03, 'b', 'a', 'r'};

static void test_complete_ping_frame(void) {
    kv_frame_t f;
    int rc = kv_parse_frame(PING_FRAME, sizeof(PING_FRAME), &f);
    KV_ASSERT_EQ_INT(rc, 1);
    KV_ASSERT_EQ_INT(f.opcode, KV_OP_PING);
    KV_ASSERT_EQ_INT(f.payload_len, 0);
    KV_ASSERT_EQ_INT(f.frame_len, KV_HEADER_LEN);
}

static void test_incomplete_header(void) {
    kv_frame_t f;
    /* Only the first 4 of 7 header bytes. */
    int rc = kv_parse_frame(SET_FOO_BAR_FRAME, 4, &f);
    KV_ASSERT_EQ_INT(rc, 0);
}

static void test_header_complete_payload_incomplete(void) {
    kv_frame_t f;
    /* Full 7-byte header plus only 3 of the SET frame's 12 payload bytes. */
    int rc = kv_parse_frame(SET_FOO_BAR_FRAME, KV_HEADER_LEN + 3, &f);
    KV_ASSERT_EQ_INT(rc, 0);
}

static void test_set_frame_fields_and_payload_bytes(void) {
    kv_frame_t f;
    int rc = kv_parse_frame(SET_FOO_BAR_FRAME, sizeof(SET_FOO_BAR_FRAME), &f);
    KV_ASSERT_EQ_INT(rc, 1);
    KV_ASSERT_EQ_INT(f.opcode, KV_OP_SET);
    KV_ASSERT_EQ_INT(f.payload_len, 12);
    KV_ASSERT_EQ_INT(f.frame_len, sizeof(SET_FOO_BAR_FRAME));
    KV_ASSERT_EQ_BYTES(f.payload, f.payload_len,
                        SET_FOO_BAR_FRAME + KV_HEADER_LEN, 12);
}

static void test_two_frames_in_one_buffer(void) {
    uint8_t buf[sizeof(PING_FRAME) + sizeof(SET_FOO_BAR_FRAME)];
    memcpy(buf, PING_FRAME, sizeof(PING_FRAME));
    memcpy(buf + sizeof(PING_FRAME), SET_FOO_BAR_FRAME,
           sizeof(SET_FOO_BAR_FRAME));

    kv_frame_t f1;
    int rc1 = kv_parse_frame(buf, sizeof(buf), &f1);
    KV_ASSERT_EQ_INT(rc1, 1);
    KV_ASSERT_EQ_INT(f1.opcode, KV_OP_PING);
    KV_ASSERT_EQ_INT(f1.frame_len, KV_HEADER_LEN);

    /* The caller advances by f1.frame_len and parses again -- this is
     * exactly what Stage 4's read loop has to do with a live socket
     * buffer that may contain more than one frame from one recv(). */
    kv_frame_t f2;
    int rc2 = kv_parse_frame(buf + f1.frame_len, sizeof(buf) - f1.frame_len, &f2);
    KV_ASSERT_EQ_INT(rc2, 1);
    KV_ASSERT_EQ_INT(f2.opcode, KV_OP_SET);
    KV_ASSERT_EQ_INT(f2.frame_len, sizeof(SET_FOO_BAR_FRAME));
}

static void test_bad_magic_is_malformed(void) {
    uint8_t buf[sizeof(PING_FRAME)];
    memcpy(buf, PING_FRAME, sizeof(buf));
    buf[0] = 0x00; /* corrupt magic */

    kv_frame_t f;
    int rc = kv_parse_frame(buf, sizeof(buf), &f);
    KV_ASSERT_EQ_INT(rc, -1);
}

static void test_oversized_payload_len_is_malformed(void) {
    uint8_t header[KV_HEADER_LEN] = {0xAB, 0x03, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};
    /* payload_len field = 0xFFFFFFFF, far beyond KV_MAX_PAYLOAD_LEN.
     * Only the 7 header bytes are given -- an oversized length must be
     * caught as soon as the header is readable, not after (impossibly)
     * waiting for that many payload bytes to arrive. */
    kv_frame_t f;
    int rc = kv_parse_frame(header, sizeof(header), &f);
    KV_ASSERT_EQ_INT(rc, -1);
}

int main(void) {
    KV_RUN(test_complete_ping_frame);
    KV_RUN(test_incomplete_header);
    KV_RUN(test_header_complete_payload_incomplete);
    KV_RUN(test_set_frame_fields_and_payload_bytes);
    KV_RUN(test_two_frames_in_one_buffer);
    KV_RUN(test_bad_magic_is_malformed);
    KV_RUN(test_oversized_payload_len_is_malformed);
    KV_REPORT_AND_EXIT();
}
