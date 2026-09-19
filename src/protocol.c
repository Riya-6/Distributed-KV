#include "kv/protocol.h"
// FRAME: |MAGIC(1B)|OPCODE(1B)|PAYLOAD_LEN(4B)|FLAGS(1B)|PAYLOAD|
// HEADER: 7B


// combine 4 big-endian bytes into a uint32_t 
static uint32_t read_u32_be(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
         | ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

int kv_parse_frame(const uint8_t *buf, size_t buf_len, kv_frame_t *out) {
    if (buf_len < KV_HEADER_LEN) return 0;  // not a full header yet
    if (buf[0] != KV_MAGIC) return -1;      

    uint32_t payload_len = read_u32_be(buf + 2); 
    if (payload_len > KV_MAX_PAYLOAD_LEN) return -1; // cannot exceed max payload size

    size_t frame_len = KV_HEADER_LEN + payload_len; //header + payload length
    if (buf_len < frame_len) return 0; // payoad not fully received yet

    out->opcode = buf[1];
    out->payload_len = payload_len;
    out->payload = payload_len ? buf + KV_HEADER_LEN : NULL;
    out->frame_len = frame_len;
    return 1;
}
