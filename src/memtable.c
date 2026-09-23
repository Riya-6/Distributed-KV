
#include "kv/memtable.h"

#include <stdlib.h>
#include <string.h>

// Each memtable entry stores a key, value, or a tombstone.
typedef struct {
    uint8_t *key;
    uint16_t key_len;
    uint8_t *value;   // NULL if is_tombstone
    uint32_t value_len;
    int is_tombstone;
} mt_entry_t;

struct kv_memtable {
    mt_entry_t *entries;
    size_t count;
    size_t capacity;
};

// Compare two keys to keep the entries sorted
static int cmp_key(const uint8_t *a, uint16_t a_len, const uint8_t *b,
                    uint16_t b_len) {
    uint16_t min_len = a_len < b_len ? a_len : b_len;
    int c = memcmp(a, b, min_len);
    if (c != 0) return c;
    if (a_len < b_len) return -1;
    if (a_len > b_len) return 1;
    return 0;
}

// Find a key or the position where it should be inserted.
static size_t find_index(const kv_memtable_t *mt, const uint8_t *key,
                          uint16_t key_len, int *found) {
    size_t lo = 0, hi = mt->count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int c = cmp_key(mt->entries[mid].key, mt->entries[mid].key_len, key,
                         key_len);
        if (c == 0) {
            *found = 1;
            return mid;
        } else if (c < 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    *found = 0;
    return lo;
}

// Create an empty memtable.
kv_memtable_t *kv_memtable_create(void) {
    return calloc(1, sizeof(struct kv_memtable));
}

// Free all entries and the memtable itself.
void kv_memtable_destroy(kv_memtable_t *mt) {
    if (mt == NULL) return;
    for (size_t i = 0; i < mt->count; i++) {
        free(mt->entries[i].key);
        free(mt->entries[i].value);
    }
    free(mt->entries);
    free(mt);
}

// Grow the entries array when it becomes full.
static int ensure_capacity(kv_memtable_t *mt) {
    if (mt->count < mt->capacity) return 0;
    size_t new_capacity = mt->capacity ? mt->capacity * 2 : 4;
    mt_entry_t *grown = realloc(mt->entries, new_capacity * sizeof(mt_entry_t));
    if (grown == NULL) return -1;
    mt->entries = grown;
    mt->capacity = new_capacity;
    return 0;
}

// Insert a new key or update an existing key.
static int upsert(kv_memtable_t *mt, const uint8_t *key, uint16_t key_len,
                   const uint8_t *value, uint32_t value_len, int is_tombstone) {
    int found;
    size_t idx = find_index(mt, key, key_len, &found);

    uint8_t *value_copy = NULL;
    if (!is_tombstone) {
        value_copy = malloc(value_len ? value_len : 1);
        if (value_copy == NULL) return -1;
        memcpy(value_copy, value, value_len);
    }

    // Update the existing entry.
    if (found) {
        free(mt->entries[idx].value);
        mt->entries[idx].value = value_copy;
        mt->entries[idx].value_len = is_tombstone ? 0 : value_len;
        mt->entries[idx].is_tombstone = is_tombstone;
        return 0;
    }

    // Make sure there is space for a new entry.
    if (ensure_capacity(mt) != 0) {
        free(value_copy);
        return -1;
    }

    uint8_t *key_copy = malloc(key_len ? key_len : 1);
    if (key_copy == NULL) {
        free(value_copy);
        return -1;
    }
    memcpy(key_copy, key, key_len);

    // Shift entries right to keep the array sorted.
    memmove(&mt->entries[idx + 1], &mt->entries[idx],
            (mt->count - idx) * sizeof(mt_entry_t));

    mt->entries[idx].key = key_copy;
    mt->entries[idx].key_len = key_len;
    mt->entries[idx].value = value_copy;
    mt->entries[idx].value_len = is_tombstone ? 0 : value_len;
    mt->entries[idx].is_tombstone = is_tombstone;
    mt->count++;
    return 0;
}

// Insert or update a normal key-value pair.
int kv_memtable_put(kv_memtable_t *mt, const uint8_t *key, uint16_t key_len,
                     const uint8_t *value, uint32_t value_len) {
    return upsert(mt, key, key_len, value, value_len, 0);
}

// Mark a key as deleted using a tombstone.
int kv_memtable_delete(kv_memtable_t *mt, const uint8_t *key, uint16_t key_len) {
    return upsert(mt, key, key_len, NULL, 0, 1);
}

// Look up a key and return its value if it exists.
kv_lookup_result_t kv_memtable_get(const kv_memtable_t *mt, const uint8_t *key,
                                    uint16_t key_len, uint8_t **out_value,
                                    uint32_t *out_value_len) {
    int found;
    size_t idx = find_index(mt, key, key_len, &found);

    if (!found) return KV_LOOKUP_MISS;

    // A tombstone means the key was explicitly deleted.
    if (mt->entries[idx].is_tombstone) return KV_LOOKUP_TOMBSTONE;

    uint32_t value_len = mt->entries[idx].value_len;
    uint8_t *copy = malloc(value_len ? value_len : 1);

    if (copy == NULL) return KV_LOOKUP_MISS;

    memcpy(copy, mt->entries[idx].value, value_len);

    *out_value = copy;
    *out_value_len = value_len;

    return KV_LOOKUP_HIT;
}

// Return the number of entries in the memtable.
size_t kv_memtable_count(const kv_memtable_t *mt) {
    return mt->count;
}

// Return a view of the entry at the given index.
int kv_memtable_entry_at(const kv_memtable_t *mt, size_t index,
                          kv_entry_view_t *out) {
    if (index >= mt->count) return -1;

    const mt_entry_t *e = &mt->entries[index];

    out->key = e->key;
    out->key_len = e->key_len;
    out->is_tombstone = e->is_tombstone;
    out->value = e->is_tombstone ? NULL : e->value;
    out->value_len = e->is_tombstone ? 0 : e->value_len;

    return 0;
}

