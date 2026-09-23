#define _POSIX_C_SOURCE 200809L 

#include "kv/sstable.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "kv/command.h"
#include "kv/protocol.h"

// Write one memtable entry to the SSTable.
static int write_entry(FILE *f, const kv_entry_view_t *view) {
    uint8_t opcode = view->is_tombstone ? KV_OP_DELETE : KV_OP_SET;
    uint32_t payload_len =
        2 + view->key_len + (view->is_tombstone ? 0 : 4 + view->value_len);

    uint8_t *payload = malloc(payload_len ? payload_len : 1);
    if (payload == NULL) return -1;

    // Store the key length and key.
    payload[0] = (uint8_t)(view->key_len >> 8);
    payload[1] = (uint8_t)view->key_len;
    memcpy(payload + 2, view->key, view->key_len);

    // Store the value length and value for normal entries.
    if (!view->is_tombstone) {
        uint32_t off = 2 + view->key_len;
        payload[off] = (uint8_t)(view->value_len >> 24);
        payload[off + 1] = (uint8_t)(view->value_len >> 16);
        payload[off + 2] = (uint8_t)(view->value_len >> 8);
        payload[off + 3] = (uint8_t)view->value_len;
        memcpy(payload + off + 4, view->value, view->value_len);
    }

    // Add the KV protocol header.
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

    // Write the complete entry to the SSTable.
    size_t written = fwrite(frame_buf, 1, frame_len, f);
    free(frame_buf);

    return (written == frame_len) ? 0 : -1;
}

// Write the entire memtable to an SSTable file.
int kv_sstable_write(const char *path, const kv_memtable_t *mt) {
    FILE *f = fopen(path, "wb");

    if (f == NULL) return -1;

    size_t count = kv_memtable_count(mt);

    // Write every entry in sorted order.
    for (size_t i = 0; i < count; i++) {
        kv_entry_view_t view;

        if (kv_memtable_entry_at(mt, i, &view) != 0) {
            fclose(f);
            return -1;
        }

        if (write_entry(f, &view) != 0) {
            fclose(f);
            return -1;
        }
    }

    // Make sure the SSTable is saved to disk.
    if (fflush(f) != 0 || fsync(fileno(f)) != 0) {
        fclose(f);
        return -1;
    }

    fclose(f);
    return 0;
}

// Read SSTable

typedef struct {
    const uint8_t *key;
    uint16_t key_len;
    size_t offset; 
    int is_tombstone;
} index_entry_t;

struct kv_sstable {
    uint8_t *file_bytes;
    size_t file_len;
    index_entry_t *index;
    size_t count;
};

// Compare two keys.
static int cmp_key(const uint8_t *a, uint16_t a_len, const uint8_t *b,
                    uint16_t b_len) {
    uint16_t min_len = a_len < b_len ? a_len : b_len;
    int c = memcmp(a, b, min_len);
    if (c != 0) return c;
    if (a_len < b_len) return -1;
    if (a_len > b_len) return 1;
    return 0;
}

// Open the SSTable and build an index for its entries.
kv_sstable_t *kv_sstable_open(const char *path) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) return NULL;

    // Find the size of the file.
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size < 0) {
        fclose(f);
        return NULL;
    }

    // Read the entire SSTable into memory.
    uint8_t *bytes = malloc((size_t)size ? (size_t)size : 1);

    if (bytes == NULL) {
        fclose(f);
        return NULL;
    }

    size_t n = fread(bytes, 1, (size_t)size, f);
    fclose(f);

    if (n != (size_t)size) {
        free(bytes);
        return NULL;
    }

    // Create the SSTable structure.
    kv_sstable_t *sst = calloc(1, sizeof(*sst));

    if (sst == NULL) {
        free(bytes);
        return NULL;
    }

    sst->file_bytes = bytes;
    sst->file_len = n;

    size_t offset = 0, capacity = 0;

    // Scan every record and add it to the index.
    while (offset < sst->file_len) {
        kv_frame_t frame;

        int prc = kv_parse_frame(
            sst->file_bytes + offset,
            sst->file_len - offset,
            &frame
        );

        if (prc != 1) {
            kv_sstable_close(sst);
            return NULL;
        }

        kv_command_t cmd;

        // Decode the record to get its key.
        if (kv_decode_command(&frame, &cmd) != 0) {
            kv_sstable_close(sst);
            return NULL;
        }

        // Grow the index when it becomes full.
        if (sst->count == capacity) {
            capacity = capacity ? capacity * 2 : 4;

            index_entry_t *grown = realloc(
                sst->index,
                capacity * sizeof(index_entry_t)
            );

            if (grown == NULL) {
                kv_sstable_close(sst);
                return NULL;
            }

            sst->index = grown;
        }

        // Store the key and location of this record.
        sst->index[sst->count].key = cmd.key;
        sst->index[sst->count].key_len = cmd.key_len;
        sst->index[sst->count].offset = offset;
        sst->index[sst->count].is_tombstone =
            (frame.opcode == KV_OP_DELETE);

        sst->count++;

        // Move to the next record.
        offset += frame.frame_len;
    }

    return sst;
}

// Free the SSTable and its index.
void kv_sstable_close(kv_sstable_t *sst) {
    if (sst == NULL) return;

    free(sst->index);
    free(sst->file_bytes);
    free(sst);
}

// Find a key in the sorted index using binary search.
static int find_index(kv_sstable_t *sst, const uint8_t *key, uint16_t key_len,
                       size_t *out_idx) {
    size_t lo = 0, hi = sst->count;

    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;

        int c = cmp_key(
            sst->index[mid].key,
            sst->index[mid].key_len,
            key,
            key_len
        );

        if (c == 0) {
            *out_idx = mid;
            return 1;
        } else if (c < 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }

    return 0;
}

// Look up a key in the SSTable.
kv_lookup_result_t kv_sstable_get(kv_sstable_t *sst, const uint8_t *key,
                                   uint16_t key_len, uint8_t **out_value,
                                   uint32_t *out_value_len) {
    size_t idx;

    if (!find_index(sst, key, key_len, &idx)) {
        return KV_LOOKUP_MISS;
    }

    // A tombstone means the key was deleted.
    if (sst->index[idx].is_tombstone) {
        return KV_LOOKUP_TOMBSTONE;
    }

    // Read the full record to get the value.
    kv_frame_t frame;

    kv_parse_frame(
        sst->file_bytes + sst->index[idx].offset,
        sst->file_len - sst->index[idx].offset,
        &frame
    );

    kv_command_t cmd;
    kv_decode_command(&frame, &cmd);

    // Copy the value so the caller owns the returned memory.
    uint8_t *copy = malloc(cmd.value_len ? cmd.value_len : 1);

    if (copy == NULL) return KV_LOOKUP_MISS;

    memcpy(copy, cmd.value, cmd.value_len);

    *out_value = copy;
    *out_value_len = cmd.value_len;

    return KV_LOOKUP_HIT;
}

// Return the number of entries in the SSTable.
size_t kv_sstable_count(const kv_sstable_t *sst) {
    return sst->count;
}

// Return an entry at the given index.
int kv_sstable_entry_at(kv_sstable_t *sst, size_t index, kv_entry_view_t *out) {
    if (index >= sst->count) return -1;

    kv_frame_t frame;

    kv_parse_frame(
        sst->file_bytes + sst->index[index].offset,
        sst->file_len - sst->index[index].offset,
        &frame
    );

    kv_command_t cmd;
    kv_decode_command(&frame, &cmd);

    // Fill the output view with the entry data.
    out->key = cmd.key;
    out->key_len = cmd.key_len;
    out->is_tombstone = sst->index[index].is_tombstone;
    out->value = out->is_tombstone ? NULL : cmd.value;
    out->value_len = out->is_tombstone ? 0 : cmd.value_len;

    return 0;
}

// SSTable Compaction
int kv_sstable_compact(const char **paths, size_t count, const char *out_path) {
    // Store all opened SSTables.
    kv_sstable_t **tables = malloc(count * sizeof(kv_sstable_t *));

    // Track the current entry in each SSTable.
    size_t *cursors = calloc(count, sizeof(size_t));

    // Check allocation.
    if (tables == NULL || cursors == NULL) {
        free(tables);
        free(cursors);
        return -1;
    }

    // Open every SSTable.
    for (size_t i = 0; i < count; i++) tables[i] = kv_sstable_open(paths[i]);

    // Store the merged result temporarily.
    kv_memtable_t *merged = kv_memtable_create();

    // Check allocation.
    if (merged == NULL) {
        for (size_t i = 0; i < count; i++) kv_sstable_close(tables[i]);
        free(tables);
        free(cursors);
        return -1;
    }

    for (;;) {
        // Find the smallest key among all current entries.
        int smallest_table = -1;
        kv_entry_view_t smallest_view;

        for (size_t i = 0; i < count; i++) {
            // Skip this table if its cursor reached the end.
            if (cursors[i] >= kv_sstable_count(tables[i])) continue;

            // Get the current entry.
            kv_entry_view_t v;
            kv_sstable_entry_at(tables[i], cursors[i], &v);

            // Keep the smallest key found so far.
            if (smallest_table == -1 ||
                cmp_key(v.key, v.key_len, smallest_view.key, smallest_view.key_len) < 0) {
                smallest_table = (int)i;
                smallest_view = v;
            }
        }

        // Stop when every SSTable is exhausted.
        if (smallest_table == -1) break;

        // Find the newest version of this key.
        int newest_table = smallest_table;
        kv_entry_view_t newest_view = smallest_view;

        for (size_t i = 0; i < count; i++) {
            // Skip this table if its cursor reached the end.
            if (cursors[i] >= kv_sstable_count(tables[i])) continue;

            // Get the current entry.
            kv_entry_view_t v;
            kv_sstable_entry_at(tables[i], cursors[i], &v);

            // Check if this table also has the same key.
            if (cmp_key(v.key, v.key_len,
                        smallest_view.key, smallest_view.key_len) == 0) {

                // Higher index means newer SSTable.
                if ((int)i >= newest_table) {
                    newest_table = (int)i;
                    newest_view = v;
                }

                // Move past this key.
                cursors[i]++;
            }
        }

        // Keep the newest value if it is not deleted.
        if (!newest_view.is_tombstone) {
            kv_memtable_put(merged,
                            newest_view.key,
                            newest_view.key_len,
                            newest_view.value,
                            newest_view.value_len);
        }

        // Drop the key if its newest version is a tombstone.
    }

    // Write the merged data into the new SSTable.
    int rc = kv_sstable_write(out_path, merged);

    // Free the temporary merged memtable.
    kv_memtable_destroy(merged);

    // Close all input SSTables.
    for (size_t i = 0; i < count; i++) kv_sstable_close(tables[i]);

    // Free the arrays.
    free(tables);
    free(cursors);

    // Return the write result.
    return rc;
}