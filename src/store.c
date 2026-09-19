#include "kv/store.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

// One entry stores one key-value pair.
typedef struct {
    uint8_t *key;
    uint16_t key_len;
    uint8_t *value;
    uint32_t value_len;
} entry_t;

// Mutex protects the shared store.
static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;

// Dynamic array of entries.
static entry_t *g_entries = NULL;

// Number of entries currently stored.
static size_t g_count = 0;

// Current size of the allocated array.
static size_t g_capacity = 0;

// Find a key while the mutex is already locked.
static entry_t *find_entry_locked(const uint8_t *key, uint16_t key_len) {
    for (size_t i = 0; i < g_count; i++) {

        if (g_entries[i].key_len == key_len &&
            memcmp(g_entries[i].key, key, key_len) == 0) {
            return &g_entries[i];
        }
    }

    return NULL;
}

int kv_store_set(const uint8_t *key, uint16_t key_len, const uint8_t *value,
                  uint32_t value_len) {

    // Allocate memory for a copy of the value.
    uint8_t *value_copy = malloc(value_len ? value_len : 1);

    if (value_copy == NULL) return -1;

    // Copy the value into store-owned memory.
    memcpy(value_copy, value, value_len);

    // Lock the store before accessing shared data.
    pthread_mutex_lock(&g_mutex);

    // Check whether the key already exists.
    entry_t *existing = find_entry_locked(key, key_len);

    // Replace the value if the key already exists.
    if (existing != NULL) {
        free(existing->value);
        existing->value = value_copy;
        existing->value_len = value_len;

        // Unlock the store.
        pthread_mutex_unlock(&g_mutex);

        // SET was successful.
        return 0;
    }

    // Grow the array if it is full.
    if (g_count == g_capacity) {

        // Start with 4 entries, then double the capacity.
        size_t new_capacity = g_capacity ? g_capacity * 2 : 4;

        // Allocate a larger array.
        entry_t *grown = realloc(
            g_entries,
            new_capacity * sizeof(entry_t)
        );

        if (grown == NULL) {
            free(value_copy);
            pthread_mutex_unlock(&g_mutex);
            return -1;
        }

        g_entries = grown;
        g_capacity = new_capacity;
    }

    uint8_t *key_copy = malloc(key_len ? key_len : 1);

    if (key_copy == NULL) {
        free(value_copy);
        pthread_mutex_unlock(&g_mutex);
        return -1;
    }

    // Copy the key into store-owned memory.
    memcpy(key_copy, key, key_len);

    // Store the new key and value.
    g_entries[g_count].key = key_copy;
    g_entries[g_count].key_len = key_len;
    g_entries[g_count].value = value_copy;
    g_entries[g_count].value_len = value_len;

    // Increase the number of stored entries.
    g_count++;

    // Unlock the store.
    pthread_mutex_unlock(&g_mutex);

    // SET was successful.
    return 0;
}

int kv_store_get(const uint8_t *key, uint16_t key_len, uint8_t **out_value,
                  uint32_t *out_value_len) {

    // Lock the store before searching.
    pthread_mutex_lock(&g_mutex);

    // Find the requested key.
    entry_t *existing = find_entry_locked(key, key_len);

    if (existing == NULL) {
        pthread_mutex_unlock(&g_mutex);
        return 0;
    }

    uint8_t *copy = malloc(existing->value_len ? existing->value_len : 1);

    if (copy == NULL) {
        pthread_mutex_unlock(&g_mutex);
        return -1;
    }

    // Copy the stored value.
    memcpy(copy, existing->value, existing->value_len);

    // Save the value length before unlocking.
    uint32_t copy_len = existing->value_len;

    // Unlock the store.
    pthread_mutex_unlock(&g_mutex);

    // Give the copied value to the caller.
    *out_value = copy;
    *out_value_len = copy_len;

    return 1;
}

int kv_store_delete(const uint8_t *key, uint16_t key_len) {

    // Lock the store before modifying it.
    pthread_mutex_lock(&g_mutex);

    // Search through all stored entries.
    for (size_t i = 0; i < g_count; i++) {

        if (g_entries[i].key_len == key_len &&
            memcmp(g_entries[i].key, key, key_len) == 0) {

            // Free the key memory.
            free(g_entries[i].key);

            // Free the value memory.
            free(g_entries[i].value);

            // Move the last entry into the deleted entry's position.
            g_entries[i] = g_entries[g_count - 1];

            // Decrease the number of entries.
            g_count--;

            // Unlock the store.
            pthread_mutex_unlock(&g_mutex);

            return 1;
        }
    }

    pthread_mutex_unlock(&g_mutex);

    return 0;
}

void kv_store_destroy(void) {

    // Lock the store before destroying it.
    pthread_mutex_lock(&g_mutex);

    // Free every stored key and value.
    for (size_t i = 0; i < g_count; i++) {
        free(g_entries[i].key);
        free(g_entries[i].value);
    }

    // Free the entries array.
    free(g_entries);

    // Reset the store.
    g_entries = NULL;
    g_count = 0;
    g_capacity = 0;

    // Unlock the store.
    pthread_mutex_unlock(&g_mutex);
}