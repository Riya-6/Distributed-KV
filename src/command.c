#include "kv/command.h"

#include <string.h>

// Read a 16-bit number stored in big-endian format.

static uint16_t read_u16_be(const uint8_t *p) {
    return (uint16_t)(
        ((uint16_t)p[0] << 8) |
        (uint16_t)p[1]
    );
}

// Read a 32-bit number stored in big-endian format.

static uint32_t read_u32_be(const uint8_t *p) {
    return
        ((uint32_t)p[0] << 24) |
        ((uint32_t)p[1] << 16) |
        ((uint32_t)p[2] << 8)  |
        (uint32_t)p[3];
}

// Write a 32-bit number in big-endian format.
 
static void write_u32_be(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

// Convert a received frame into a command.

int kv_decode_command(const kv_frame_t *frame, kv_command_t *out) {
    // Clear the output structure before filling it.
    memset(out, 0, sizeof(*out));

    // Check which operation the client requested.
    switch (frame->opcode) {

    // PING does not contain any payload.   
    case KV_OP_PING:
        out->type = KV_CMD_PING;
        return 0;

    /*
     * GET and DELETE use the same payload format:
     * key_len: 2 bytes key: key_len bytes
     */
    case KV_OP_GET:
    case KV_OP_DELETE: {

        // The payload must contain at least key_len.
        if (frame->payload_len < 2) {
            return -1;
        }
        // Read the key length from the first 2 bytes.
        uint16_t key_len = read_u16_be(frame->payload);

        /*
         * Check that the payload contains exactly:
         *  2 bytes for key_len and key_len bytes for the key
         */
        if ((size_t)2 + key_len != frame->payload_len) {
            return -1;
        }

        out->type =
            (frame->opcode == KV_OP_GET)
                ? KV_CMD_GET
                : KV_CMD_DELETE;

        out->key = frame->payload + 2;
        out->key_len = key_len;

        return 0;
    }

    /*
     * SET uses the payload format:
     *
     *   key_len:   2 bytes
     *   key:       key_len bytes
     *   value_len: 4 bytes
     *   value:     value_len bytes
     */
    case KV_OP_SET: {

        if (frame->payload_len < 2) {
            return -1;
        }
        uint16_t key_len = read_u16_be(frame->payload);

        // The value length comes after 2 bytes for key_len and key_len bytes for the key
        size_t value_len_off = (size_t)2 + key_len;

        if (value_len_off + 4 > frame->payload_len) {
            return -1;
        }

        uint32_t value_len =
            read_u32_be(frame->payload + value_len_off);
        size_t value_off = value_len_off + 4;

        // Check that the payload contains exactly the declared number of value bytes.
         
        if (value_off + value_len != frame->payload_len) {
            return -1;
        }

        out->type = KV_CMD_SET;
        out->key = frame->payload + 2;
        out->key_len = key_len;
        out->value = frame->payload + value_off;
        out->value_len = value_len;

        return 0;
    }
    default:
        return -1;
    }
}

// Create a response frame in the output buffer.


size_t kv_encode_response(
    uint8_t opcode,
    const uint8_t *payload,
    uint32_t payload_len,
    uint8_t *out_buf,
    size_t out_buf_cap
) {
    // Calculate the complete frame size.
    size_t total = KV_HEADER_LEN + payload_len;

    //Do not write if the buffer is too small.
    if (out_buf_cap < total) {
        return 0;
    }

    out_buf[0] = KV_MAGIC;
    out_buf[1] = opcode;
    write_u32_be(out_buf + 2, payload_len);
    out_buf[6] = 0x00;
    if (payload_len > 0) {
        memcpy(
            out_buf + KV_HEADER_LEN,
            payload,
            payload_len
        );
    }

    // Return the number of bytes in the response frame.
    return total;
}