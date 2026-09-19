#ifndef KV_PROTOCOL_H
#define KV_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#define KV_MAGIC 0xAB
#define KV_HEADER_LEN 7u
#define KV_MAX_PAYLOAD_LEN (16u * 1024u * 1024u) /* 16 MiB */

/* Request opcodes */
#define KV_OP_PING 0x01
#define KV_OP_GET 0x02
#define KV_OP_SET 0x03
#define KV_OP_DELETE 0x04

/* Response opcodes */
#define KV_OP_PONG 0x81
#define KV_OP_OK 0x82
#define KV_OP_VALUE 0x83
#define KV_OP_NOT_FOUND 0x84
#define KV_OP_ERR 0x85

typedef struct {
    uint8_t opcode;
    uint32_t payload_len;
    const uint8_t *payload; 
    size_t frame_len;       
} kv_frame_t;


int kv_parse_frame(const uint8_t *buf, size_t buf_len, kv_frame_t *out);

#endif 
