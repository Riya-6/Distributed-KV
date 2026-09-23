#ifndef KV_MEMTABLE_H
#define KV_MEMTABLE_H

#include <stddef.h>
#include <stdint.h>

typedef struct kv_memtable kv_memtable_t;

typedef enum {
    KV_LOOKUP_MISS = 0,      // key not present at all 
    KV_LOOKUP_HIT = 1,       // key present with a live value
    KV_LOOKUP_TOMBSTONE = 2  // key present but deleted, callers merging across layers must stop searching
} kv_lookup_result_t;

// A read-only view into one entry, by sorted position 
typedef struct {
    const uint8_t *key;
    uint16_t key_len;
    const uint8_t *value;   
    uint32_t value_len;     
    int is_tombstone;
} kv_entry_view_t;

kv_memtable_t *kv_memtable_create(void);

void kv_memtable_destroy(kv_memtable_t *mt);


int kv_memtable_put(kv_memtable_t *mt, const uint8_t *key, uint16_t key_len,
                     const uint8_t *value, uint32_t value_len);

// Mark `key` as deleted (a tombstone), copying the key in if it wasn't already present. 
int kv_memtable_delete(kv_memtable_t *mt, const uint8_t *key, uint16_t key_len);


kv_lookup_result_t kv_memtable_get(const kv_memtable_t *mt, const uint8_t *key,
                                    uint16_t key_len, uint8_t **out_value,
                                    uint32_t *out_value_len);


size_t kv_memtable_count(const kv_memtable_t *mt);


int kv_memtable_entry_at(const kv_memtable_t *mt, size_t index,
                          kv_entry_view_t *out);

#endif
