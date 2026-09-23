#define _POSIX_C_SOURCE 200809L 

#include "kv/wal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "kv/command.h"
#include "kv/protocol.h"


struct kv_wal {
    FILE *f;
};

// Open the WAL file, creating it if needed.
kv_wal_t *kv_wal_open(const char *path) {
    FILE *f = fopen(path, "a+b");
    if (f == NULL) return NULL;

    kv_wal_t *wal = malloc(sizeof(*wal));
    if (wal == NULL) {
        fclose(f);
        return NULL;
    }
    wal->f = f;
    return wal;
}

// Close the WAL file and free the WAL structure.
void kv_wal_close(kv_wal_t *wal) {
    if (wal == NULL) return;
    fclose(wal->f);
    free(wal);
}

// Build a SET or DELETE record and append it to the WAL. 
static int append_record(kv_wal_t *wal, uint8_t opcode, const uint8_t *key,
                          uint16_t key_len, const uint8_t *value,
                          uint32_t value_len) {
    int has_value = (opcode == KV_OP_SET);
    uint32_t payload_len = 2 + key_len + (has_value ? 4 + value_len : 0);

    uint8_t *payload = malloc(payload_len ? payload_len : 1);
    if (payload == NULL) return -1;

    // Store the key length and key.
    payload[0] = (uint8_t)(key_len >> 8);
    payload[1] = (uint8_t)key_len;
    memcpy(payload + 2, key, key_len);

    // Store the value length and value for SET.
    if (has_value) {
        uint32_t off = 2 + key_len;
        payload[off] = (uint8_t)(value_len >> 24);
        payload[off + 1] = (uint8_t)(value_len >> 16);
        payload[off + 2] = (uint8_t)(value_len >> 8);
        payload[off + 3] = (uint8_t)value_len;
        memcpy(payload + off + 4, value, value_len);
    }

    // Add the normal KV protocol header.
    size_t frame_cap = KV_HEADER_LEN + payload_len;
    uint8_t *frame_buf = malloc(frame_cap);
    if (frame_buf == NULL) {
        free(payload);
        return -1;
    }

    size_t frame_len =
        kv_encode_response(opcode, payload, payload_len, frame_buf, frame_cap);

    free(payload);

    if (frame_len == 0) {
        free(frame_buf);
        return -1;
    }

    // Write the complete record to the WAL.
    size_t written = fwrite(frame_buf, 1, frame_len, wal->f);
    free(frame_buf);

    if (written != frame_len) return -1;

    // Make sure the record is actually saved to disk.
    if (fflush(wal->f) != 0) return -1;
    if (fsync(fileno(wal->f)) != 0) return -1;

    return 0;
}

// Add a SET operation to the WAL.
int kv_wal_append_set(kv_wal_t *wal, const uint8_t *key, uint16_t key_len,
                       const uint8_t *value, uint32_t value_len) {
    return append_record(wal, KV_OP_SET, key, key_len, value, value_len);
}

// Add a DELETE operation to the WAL.
int kv_wal_append_delete(kv_wal_t *wal, const uint8_t *key, uint16_t key_len) {
    return append_record(wal, KV_OP_DELETE, key, key_len, NULL, 0);
}

// Clear the WAL file.
int kv_wal_truncate(kv_wal_t *wal) {
    if (ftruncate(fileno(wal->f), 0) != 0) return -1;
    rewind(wal->f); /* reset the file position */
    return 0;
}

// Read the WAL and replay all stored operations.
int kv_wal_replay(const char *path, kv_wal_replay_set_fn on_set,
                   kv_wal_replay_delete_fn on_delete, void *ctx) {
    FILE *f = fopen(path, "rb");

    // No WAL means there is nothing to recover.
    if (f == NULL) return 0;

    // Find the size of the WAL file.
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size < 0) {
        fclose(f);
        return -1;
    }

    // Read the entire WAL into memory.
    uint8_t *buf = malloc((size_t)size ? (size_t)size : 1);

    if (buf == NULL) {
        fclose(f);
        return -1;
    }

    size_t n = fread(buf, 1, (size_t)size, f);
    fclose(f);

    if (n != (size_t)size) {
        free(buf);
        return -1;
    }

    size_t offset = 0;

    // Process each WAL record one by one.
    while (offset < n) {
        kv_frame_t frame;
        int prc = kv_parse_frame(buf + offset, n - offset, &frame);

        if (prc != 1) { /* incomplete or malformed record */
            free(buf);
            return -1;
        }

        kv_command_t cmd;

        // Decode the record back into a command.
        if (kv_decode_command(&frame, &cmd) != 0) {
            free(buf);
            return -1;
        }

        // Replay the original operation.
        if (frame.opcode == KV_OP_SET) {
            on_set(cmd.key, cmd.key_len, cmd.value, cmd.value_len, ctx);
        } else if (frame.opcode == KV_OP_DELETE) {
            on_delete(cmd.key, cmd.key_len, ctx);
        } else {
            free(buf);
            return -1; // WAL should only contain SET/DELETE 
        }

        // Move to the next record.
        offset += frame.frame_len;
    }

    free(buf);
    return 0;
}