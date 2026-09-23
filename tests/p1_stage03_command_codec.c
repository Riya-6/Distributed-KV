/*
 * Phase 1, Stage 3: command encode/decode (pure functions, no sockets).
 * Contract under test: include/kv/command.h -> src/command.c (you write this).
 * See docs/stages/phase1-tcp-server.md, Stage 3.
 */

#include <string.h>

#include "kv/command.h"
#include "kv/protocol.h"
#include "test_framework.h"

static void test_decode_ping(void) {
    kv_frame_t frame = {.opcode = KV_OP_PING, .flags = 0, .payload_len = 0,
                         .payload = NULL, .frame_len = KV_HEADER_LEN};
    kv_command_t cmd;
    int rc = kv_decode_command(&frame, &cmd);
    KV_ASSERT_EQ_INT(rc, 0);
    KV_ASSERT_EQ_INT(cmd.type, KV_CMD_PING);
}

static void test_decode_get(void) {
    /* key_len=3, "foo" */
    static const uint8_t payload[] = {0x00, 0x03, 'f', 'o', 'o'};
    kv_frame_t frame = {.opcode = KV_OP_GET, .flags = 0,
                         .payload_len = sizeof(payload), .payload = payload,
                         .frame_len = KV_HEADER_LEN + sizeof(payload)};
    kv_command_t cmd;
    int rc = kv_decode_command(&frame, &cmd);
    KV_ASSERT_EQ_INT(rc, 0);
    KV_ASSERT_EQ_INT(cmd.type, KV_CMD_GET);
    KV_ASSERT_EQ_INT(cmd.key_len, 3);
    KV_ASSERT_EQ_BYTES(cmd.key, cmd.key_len, "foo", 3);
}

static void test_decode_delete(void) {
    static const uint8_t payload[] = {0x00, 0x03, 'f', 'o', 'o'};
    kv_frame_t frame = {.opcode = KV_OP_DELETE, .flags = 0,
                         .payload_len = sizeof(payload), .payload = payload,
                         .frame_len = KV_HEADER_LEN + sizeof(payload)};
    kv_command_t cmd;
    int rc = kv_decode_command(&frame, &cmd);
    KV_ASSERT_EQ_INT(rc, 0);
    KV_ASSERT_EQ_INT(cmd.type, KV_CMD_DELETE);
    KV_ASSERT_EQ_BYTES(cmd.key, cmd.key_len, "foo", 3);
}

static void test_decode_set(void) {
    /* key_len=3 "foo", value_len=3 "bar" */
    static const uint8_t payload[] = {0x00, 0x03, 'f',  'o',  'o',
                                       0x00, 0x00, 0x00, 0x03, 'b', 'a', 'r'};
    kv_frame_t frame = {.opcode = KV_OP_SET, .flags = 0,
                         .payload_len = sizeof(payload), .payload = payload,
                         .frame_len = KV_HEADER_LEN + sizeof(payload)};
    kv_command_t cmd;
    int rc = kv_decode_command(&frame, &cmd);
    KV_ASSERT_EQ_INT(rc, 0);
    KV_ASSERT_EQ_INT(cmd.type, KV_CMD_SET);
    KV_ASSERT_EQ_BYTES(cmd.key, cmd.key_len, "foo", 3);
    KV_ASSERT_EQ_INT(cmd.value_len, 3);
    KV_ASSERT_EQ_BYTES(cmd.value, cmd.value_len, "bar", 3);
}

static void test_decode_set_with_impossible_key_len_fails(void) {
    /* key_len claims 0xFFFF bytes, but the payload is nowhere near that
     * long -- must be rejected, not read out of bounds. */
    static const uint8_t payload[] = {0xFF, 0xFF, 'f',  'o',  'o',
                                       0x00, 0x00, 0x00, 0x03, 'b', 'a', 'r'};
    kv_frame_t frame = {.opcode = KV_OP_SET, .flags = 0,
                         .payload_len = sizeof(payload), .payload = payload,
                         .frame_len = KV_HEADER_LEN + sizeof(payload)};
    kv_command_t cmd;
    int rc = kv_decode_command(&frame, &cmd);
    KV_ASSERT_EQ_INT(rc, -1);
}

static void test_encode_value_response_round_trips_through_parser(void) {
    uint8_t out[64];
    size_t n = kv_encode_response(KV_OP_VALUE, (const uint8_t *)"bar", 3, out,
                                   sizeof(out));
    KV_ASSERT_EQ_INT(n, KV_HEADER_LEN + 3);

    kv_frame_t f;
    int rc = kv_parse_frame(out, n, &f);
    KV_ASSERT_EQ_INT(rc, 1);
    KV_ASSERT_EQ_INT(f.opcode, KV_OP_VALUE);
    KV_ASSERT_EQ_INT(f.payload_len, 3);
    KV_ASSERT_EQ_BYTES(f.payload, f.payload_len, "bar", 3);
}

static void test_encode_empty_payload_response(void) {
    uint8_t out[16];
    size_t n = kv_encode_response(KV_OP_OK, NULL, 0, out, sizeof(out));
    KV_ASSERT_EQ_INT(n, KV_HEADER_LEN);

    kv_frame_t f;
    int rc = kv_parse_frame(out, n, &f);
    KV_ASSERT_EQ_INT(rc, 1);
    KV_ASSERT_EQ_INT(f.opcode, KV_OP_OK);
    KV_ASSERT_EQ_INT(f.payload_len, 0);
}

static void test_encode_rejects_undersized_buffer(void) {
    uint8_t out[3]; /* smaller than even the 7-byte header */
    size_t n = kv_encode_response(KV_OP_OK, NULL, 0, out, sizeof(out));
    KV_ASSERT_EQ_INT(n, 0);
}

int main(void) {
    KV_RUN(test_decode_ping);
    KV_RUN(test_decode_get);
    KV_RUN(test_decode_delete);
    KV_RUN(test_decode_set);
    KV_RUN(test_decode_set_with_impossible_key_len_fails);
    KV_RUN(test_encode_value_response_round_trips_through_parser);
    KV_RUN(test_encode_empty_payload_response);
    KV_RUN(test_encode_rejects_undersized_buffer);
    KV_REPORT_AND_EXIT();
}
