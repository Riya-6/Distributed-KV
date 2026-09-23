#define _POSIX_C_SOURCE 200809L /* strdup */

#include "kv/store.h"

#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kv/memtable.h"
#include "kv/sstable.h"
#include "kv/wal.h"

// Flush the memtable 
#define FLUSH_THRESHOLD 8

#define WAL_FILENAME "wal.log"
#define SSTABLE_PREFIX "sstable_"

static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;

// Directory where the WAL and SSTables are stored. 
static char g_data_dir[512];

static kv_memtable_t *g_memtable = NULL;

static kv_wal_t *g_wal = NULL;

static kv_sstable_t **g_sstables = NULL;
static size_t g_sstable_count = 0;

// Index used when creating new SSTable files. 
static int g_next_sstable_index = 0;

// Replay a SET record into the memtable. 
static void replay_set_cb(const uint8_t *key, uint16_t key_len,
                          const uint8_t *value, uint32_t value_len,
                          void *ctx) {
    kv_memtable_put(
        (kv_memtable_t *)ctx,
        key,
        key_len,
        value,
        value_len
    );
}

// Replay a DELETE record into the memtable.
static void replay_delete_cb(const uint8_t *key, uint16_t key_len,
                             void *ctx) {
    kv_memtable_delete(
        (kv_memtable_t *)ctx,
        key,
        key_len
    );
}

// Compare two filenames for sorting. 
static int cmp_str(const void *a, const void *b) {
    return strcmp(
        *(const char *const *)a,
        *(const char *const *)b
    );
}

int kv_store_open(const char *data_dir) {

    // Save the data directory path. 
    strncpy(
        g_data_dir,
        data_dir,
        sizeof(g_data_dir) - 1
    );

    g_data_dir[sizeof(g_data_dir) - 1] = '\0';

    // Create an empty memtable.
    g_memtable = kv_memtable_create();

    if (g_memtable == NULL) {
        return -1;
    }

    // Build the WAL file path.
    char wal_path[600];

    snprintf(
        wal_path,
        sizeof(wal_path),
        "%s/%s",
        data_dir,
        WAL_FILENAME
    );

    /*
     * Replay the WAL before opening it for new writes.
     * This restores data that was not flushed to an SSTable.
     */
    if (kv_wal_replay(
            wal_path,
            replay_set_cb,
            replay_delete_cb,
            g_memtable) != 0) {

        kv_memtable_destroy(g_memtable);
        g_memtable = NULL;

        return -1;
    }

    // Open the WAL so new writes can be appended. 
    g_wal = kv_wal_open(wal_path);

    if (g_wal == NULL) {
        kv_memtable_destroy(g_memtable);
        g_memtable = NULL;

        return -1;
    }

    // Store the names of existing SSTable files.
    char **names = NULL;
    size_t names_count = 0;
    size_t names_cap = 0;

    // Open the data directory.
    DIR *d = opendir(data_dir);

    if (d != NULL) {
        struct dirent *entry;

        // Look through every file in the directory.
        while ((entry = readdir(d)) != NULL) {

            // Ignore files that are not SSTables.
            if (strncmp(
                    entry->d_name,
                    SSTABLE_PREFIX,
                    strlen(SSTABLE_PREFIX)) != 0) {
                continue;
            }

            // Grow the filename array if needed.
            if (names_count == names_cap) {
                names_cap = names_cap ? names_cap * 2 : 4;

                names = realloc(
                    names,
                    names_cap * sizeof(char *)
                );
            }

            // Save the SSTable filename.
            names[names_count++] = strdup(entry->d_name);
        }

        closedir(d);
    }

    // Sort SSTables from oldest to newest.
    qsort(
        names,
        names_count,
        sizeof(char *),
        cmp_str
    );

    // Allocate the SSTable pointer array.
    g_sstables = names_count
        ? malloc(names_count * sizeof(kv_sstable_t *))
        : NULL;

    g_sstable_count = 0;

    // Open every existing SSTable.
    for (size_t i = 0; i < names_count; i++) {

        char path[700];

        snprintf(
            path,
            sizeof(path),
            "%s/%s",
            data_dir,
            names[i]
        );

        kv_sstable_t *sst = kv_sstable_open(path);

        if (sst != NULL) {
            g_sstables[g_sstable_count++] = sst;
        }

        free(names[i]);
    }

    free(names);

    // Continue numbering after the existing SSTables. 
    g_next_sstable_index = (int)g_sstable_count;

    return 0;
}

/*
 * Write the current memtable to an SSTable.
 * Caller must already hold g_mutex.
 */
static void flush_memtable_locked(void) {

    // Create the next SSTable filename.
    char path[700];

    snprintf(
        path,
        sizeof(path),
        "%s/%s%04d.sst",
        g_data_dir,
        SSTABLE_PREFIX,
        g_next_sstable_index++
    );

    // Write the current memtable to disk.
    kv_sstable_write(path, g_memtable);

    // Open the newly created SSTable.
    kv_sstable_t *sst = kv_sstable_open(path);

    // Grow the SSTable array.
    kv_sstable_t **grown =
        realloc(
            g_sstables,
            (g_sstable_count + 1) * sizeof(kv_sstable_t *)
        );

    g_sstables = grown;

    // Add the new SSTable at the end.
    g_sstables[g_sstable_count++] = sst;

    // Replace the old memtable with an empty one.
    kv_memtable_destroy(g_memtable);
    g_memtable = kv_memtable_create();

    // The SSTable now contains everything from the WAL.
    kv_wal_truncate(g_wal);
}

int kv_store_set(
    const uint8_t *key,
    uint16_t key_len,
    const uint8_t *value,
    uint32_t value_len
) {
    // Lock the store before modifying it.
    pthread_mutex_lock(&g_mutex);

    /*
     * Write to the WAL first.
     * This makes the write durable before updating memory.
     */
    if (kv_wal_append_set(
            g_wal,
            key,
            key_len,
            value,
            value_len) != 0) {

        pthread_mutex_unlock(&g_mutex);
        return -1;
    }

    // Add the key-value pair to the memtable.
    int rc = kv_memtable_put(
        g_memtable,
        key,
        key_len,
        value,
        value_len
    );

    // Flush when the memtable becomes large enough.
    if (rc == 0 &&
        kv_memtable_count(g_memtable) >= FLUSH_THRESHOLD) {

        flush_memtable_locked();
    }

    pthread_mutex_unlock(&g_mutex);

    return rc;
}

int kv_store_get(
    const uint8_t *key,
    uint16_t key_len,
    uint8_t **out_value,
    uint32_t *out_value_len
) {
    // Lock while reading the store.
    pthread_mutex_lock(&g_mutex);

    // Search the memtable first because it has the newest data.
     
    kv_lookup_result_t r =
        kv_memtable_get(
            g_memtable,
            key,
            key_len,
            out_value,
            out_value_len
        );

    // Key was found in the memtable.
    if (r == KV_LOOKUP_HIT) {
        pthread_mutex_unlock(&g_mutex);
        return 1;
    }

    // A tombstone means the key was deleted.
    if (r == KV_LOOKUP_TOMBSTONE) {
        pthread_mutex_unlock(&g_mutex);
        return 0;
    }

    // Search SSTables from newest to oldest.
     
    for (size_t i = g_sstable_count; i-- > 0;) {

        kv_lookup_result_t sr =
            kv_sstable_get(
                g_sstables[i],
                key,
                key_len,
                out_value,
                out_value_len
            );

        // Found the newest available value.
        if (sr == KV_LOOKUP_HIT) {
            pthread_mutex_unlock(&g_mutex);
            return 1;
        }

        // A tombstone hides older values.
        if (sr == KV_LOOKUP_TOMBSTONE) {
            pthread_mutex_unlock(&g_mutex);
            return 0;
        }
    }

    // Key was not found anywhere.
    pthread_mutex_unlock(&g_mutex);

    return 0;
}

int kv_store_delete(
    const uint8_t *key,
    uint16_t key_len
) {
    // Lock the store before modifying it.
    pthread_mutex_lock(&g_mutex);

    
    uint8_t *tmp_val = NULL;
    uint32_t tmp_len = 0;
    int exists = 0;

    // Check the memtable first.
    kv_lookup_result_t r =
        kv_memtable_get(
            g_memtable,
            key,
            key_len,
            &tmp_val,
            &tmp_len
        );

    // Key exists in the memtable.
    if (r == KV_LOOKUP_HIT) {
        exists = 1;
        free(tmp_val);

    // If not found, check the SSTables.
    } else if (r == KV_LOOKUP_MISS) {

        // Search newest SSTable first.
        for (size_t i = g_sstable_count; i-- > 0;) {

            kv_lookup_result_t sr =
                kv_sstable_get(
                    g_sstables[i],
                    key,
                    key_len,
                    &tmp_val,
                    &tmp_len
                );

            // Key exists in an SSTable.
            if (sr == KV_LOOKUP_HIT) {
                exists = 1;
                free(tmp_val);
                break;
            }

            /*
             * A tombstone means the key was already deleted,
             * so older SSTables should not be checked.
             */
            if (sr == KV_LOOKUP_TOMBSTONE) {
                break;
            }
        }
    }

    if (!exists) {
        pthread_mutex_unlock(&g_mutex);
        return 0;
    }

    // Record the deletion in the WAL first.
     
    kv_wal_append_delete(
        g_wal,
        key,
        key_len
    );

    // Add a tombstone to the memtable.
    kv_memtable_delete(
        g_memtable,
        key,
        key_len
    );

    // Flush if the memtable reached the threshold.
    if (kv_memtable_count(g_memtable) >= FLUSH_THRESHOLD) {
        flush_memtable_locked();
    }

    pthread_mutex_unlock(&g_mutex);

    return 1;
}

void kv_store_destroy(void) {

    // Lock before destroying shared state.
    pthread_mutex_lock(&g_mutex);

    // Free the current memtable.
    kv_memtable_destroy(g_memtable);
    g_memtable = NULL;

    // Close the WAL.
    kv_wal_close(g_wal);
    g_wal = NULL;

    // Close every SSTable.
    for (size_t i = 0; i < g_sstable_count; i++) {
        kv_sstable_close(g_sstables[i]);
    }

    // Free the SSTable pointer array.
    free(g_sstables);

    g_sstables = NULL;
    g_sstable_count = 0;

    pthread_mutex_unlock(&g_mutex);
}