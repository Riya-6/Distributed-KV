#ifndef KV_COMMAND_H
#define KV_COMMAND_H

#include <stddef.h>
#include <stdint.h>

#include "kv/protocol.h"

typedef enum {
    KV_CMD_PING,
    KV_CMD_GET,
    KV_CMD_SET,
    KV_CMD_DELETE
} kv_cmd_type_t;

typedef struct {
    kv_cmd_type_t type;
    const uint8_t *key;    
    uint16_t key_len;
    const uint8_t *value;  
    uint32_t value_len;
} kv_command_t;


int kv_decode_command(const kv_frame_t *frame, kv_command_t *out);

size_t kv_encode_response(uint8_t opcode, const uint8_t *payload,
                           uint32_t payload_len, uint8_t *out_buf,
                           size_t out_buf_cap);

#endif
