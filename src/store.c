#define _POSIX_C_SOURCE 200809L /* strdup */

#include "kv/store.h"

#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "kv/memtable.h"
#include "kv/sstable.h"
#include "kv/wal.h"

// Flush the memtable
#define FLUSH_THRESHOLD 8

//  how often the active sweep thread scans the memtable for expired keys.
#define SWEEP_INTERVAL_MS 200

#define WAL_FILENAME "wal.log"
#define SSTABLE_PREFIX "sstable_"


static pthread_rwlock_t g_lock = PTHREAD_RWLOCK_INITIALIZER;

// Directory where the WAL and SSTables are stored.
static char g_data_dir[512];

static kv_memtable_t *g_memtable = NULL;

static kv_wal_t *g_wal = NULL;

static kv_sstable_t **g_sstables = NULL;
static size_t g_sstable_count = 0;

// Index used when creating new SSTable files.
static int g_next_sstable_index = 0;

// Active sweep thread state.
static pthread_t g_sweep_thread;
static int g_sweep_running = 0;
static pthread_mutex_t g_sweep_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_sweep_cond = PTHREAD_COND_INITIALIZER;
static int g_sweep_stop = 0;
static void *sweep_thread_fn(void *arg);

// Replay a SET record into the memtable.
static void replay_set_cb(const uint8_t *key, uint16_t key_len,
                          const uint8_t *value, uint32_t value_len,
                          uint32_t expires_at, void *ctx) {
    kv_memtable_put_ttl(
        (kv_memtable_t *)ctx,
        key,
        key_len,
        value,
        value_len,
        expires_at
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

    // Replay WAL before opening it for new writes, restores data that was not flushed to SSTable.
    
    if (kv_wal_replay_ttl(
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

    // Start the active sweep thread.
    g_sweep_stop = 0;
    g_sweep_running = (pthread_create(&g_sweep_thread, NULL, sweep_thread_fn, NULL) == 0);

    return 0;
}

// One pass over the memtable: any live entry whose expires_at has
// passed gets shadowed by a fresh tombstone.
static void sweep_once_locked(void) {
    uint32_t now = (uint32_t)time(NULL);
    size_t count = kv_memtable_count(g_memtable);

    for (size_t i = 0; i < count; i++) {
        kv_entry_view_t view;

        if (kv_memtable_entry_at(g_memtable, i, &view) != 0) {
            continue;
        }

        if (view.is_tombstone) {
            continue;
        }

        if (view.expires_at != 0 && view.expires_at <= now) {
            kv_wal_append_delete(g_wal, view.key, view.key_len);
            kv_memtable_delete(g_memtable, view.key, view.key_len);
        }
    }
}

static void *sweep_thread_fn(void *arg) {
    (void)arg;

    for (;;) {
        pthread_mutex_lock(&g_sweep_mutex);

        if (!g_sweep_stop) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += (long)SWEEP_INTERVAL_MS * 1000000L;
            if (ts.tv_nsec >= 1000000000L) {
                ts.tv_sec += 1;
                ts.tv_nsec -= 1000000000L;
            }
            // Ignoring the return value: a timeout means "sweep now"
            pthread_cond_timedwait(&g_sweep_cond, &g_sweep_mutex, &ts);
        }

        int stop = g_sweep_stop;
        pthread_mutex_unlock(&g_sweep_mutex);

        if (stop) {
            break;
        }

        pthread_rwlock_wrlock(&g_lock);
        sweep_once_locked();
        pthread_rwlock_unlock(&g_lock);
    }

    return NULL;
}

/*
 * Write the current memtable to an SSTable.
 * Caller must already hold the write lock (g_lock).
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

// ttl_seconds == 0 means "never expires". A positive value is
// converted to an absolute wall-clock expiry here
int kv_store_set_ttl(
    const uint8_t *key,
    uint16_t key_len,
    const uint8_t *value,
    uint32_t value_len,
    uint32_t ttl_seconds
) {
    uint32_t expires_at = ttl_seconds > 0
        ? (uint32_t)time(NULL) + ttl_seconds
        : 0;

    // Lock the store before modifying it.
    pthread_rwlock_wrlock(&g_lock);

    // Write to the WAL first. This makes the write durable before updating memory.

    if (kv_wal_append_set_ttl(
            g_wal,
            key,
            key_len,
            value,
            value_len,
            expires_at) != 0) {

        pthread_rwlock_unlock(&g_lock);
        return -1;
    }

    // Add the key-value pair to the memtable.
    int rc = kv_memtable_put_ttl(
        g_memtable,
        key,
        key_len,
        value,
        value_len,
        expires_at
    );

    // Flush when the memtable becomes large enough.
    if (rc == 0 &&
        kv_memtable_count(g_memtable) >= FLUSH_THRESHOLD) {

        flush_memtable_locked();
    }

    pthread_rwlock_unlock(&g_lock);

    return rc;
}

int kv_store_set(
    const uint8_t *key,
    uint16_t key_len,
    const uint8_t *value,
    uint32_t value_len
) {
    return kv_store_set_ttl(key, key_len, value, value_len, 0);
}

/*
 * Looks up key across the memtable then SSTables (newest to oldest).
 * Returns:
 *   1 - live HIT; out_value / out_value_len are set, caller owns the copy.
 *   0 - genuinely not found (miss or tombstone).
 *   2 - found, but its expires_at is in the past.
 */
static int lookup_locked(
    const uint8_t *key,
    uint16_t key_len,
    uint8_t **out_value,
    uint32_t *out_value_len,
    int allow_clear
) {
    uint32_t now = (uint32_t)time(NULL);
    uint32_t expires_at = 0;

    kv_lookup_result_t r =
        kv_memtable_get_ttl(g_memtable, key, key_len, out_value, out_value_len,
                             &expires_at);

    if (r == KV_LOOKUP_HIT) {
        if (expires_at != 0 && expires_at <= now) {
            free(*out_value);
            *out_value = NULL;
            if (!allow_clear) {
                return 2;
            }
            kv_wal_append_delete(g_wal, key, key_len);
            kv_memtable_delete(g_memtable, key, key_len);
            return 0;
        }
        return 1;
    }

    if (r == KV_LOOKUP_TOMBSTONE) {
        return 0;
    }

    // Search SSTables from newest to oldest.
    for (size_t i = g_sstable_count; i-- > 0;) {

        kv_lookup_result_t sr =
            kv_sstable_get_ttl(g_sstables[i], key, key_len, out_value,
                                out_value_len, &expires_at);

        if (sr == KV_LOOKUP_HIT) {
            if (expires_at != 0 && expires_at <= now) {
                free(*out_value);
                *out_value = NULL;
                if (!allow_clear) {
                    return 2;
                }
                // Shadow the still-physically-present SSTable value
                // with a fresh memtable tombstone, same mechanism a
                // real DELETE already uses.
                kv_wal_append_delete(g_wal, key, key_len);
                kv_memtable_delete(g_memtable, key, key_len);
                return 0;
            }
            return 1;
        }

        if (sr == KV_LOOKUP_TOMBSTONE) {
            return 0;
        }
    }

    return 0;
}

int kv_store_get(
    const uint8_t *key,
    uint16_t key_len,
    uint8_t **out_value,
    uint32_t *out_value_len
) {
    // Fast path: shared read lock, no mutation. Covers every case
    // except "found, but expired"
    pthread_rwlock_rdlock(&g_lock);
    int rc = lookup_locked(key, key_len, out_value, out_value_len, 0);
    pthread_rwlock_unlock(&g_lock);

    if (rc != 2) {
        return rc;
    }

    // Slow path: an expired-but-still-present entry was seen above.
    // Re-run the whole lookup under the write lock so it's safe to
    // clear it
    pthread_rwlock_wrlock(&g_lock);
    rc = lookup_locked(key, key_len, out_value, out_value_len, 1);
    pthread_rwlock_unlock(&g_lock);

    return (rc == 1) ? 1 : 0;
}

int kv_store_delete(
    const uint8_t *key,
    uint16_t key_len
) {
    // Lock the store before modifying it.
    pthread_rwlock_wrlock(&g_lock);


    uint8_t *tmp_val = NULL;
    uint32_t tmp_len = 0;
    uint32_t tmp_expires_at = 0;
    uint32_t now = (uint32_t)time(NULL);
    int exists = 0;

    // Check the memtable first.
    kv_lookup_result_t r =
        kv_memtable_get_ttl(
            g_memtable,
            key,
            key_len,
            &tmp_val,
            &tmp_len,
            &tmp_expires_at
        );

    // Key exists in the memtable, and isn't expired.
    if (r == KV_LOOKUP_HIT) {
        exists = (tmp_expires_at == 0 || tmp_expires_at > now);
        free(tmp_val);

    // If not found, check the SSTables.
    } else if (r == KV_LOOKUP_MISS) {

        // Search newest SSTable first.
        for (size_t i = g_sstable_count; i-- > 0;) {

            kv_lookup_result_t sr =
                kv_sstable_get_ttl(
                    g_sstables[i],
                    key,
                    key_len,
                    &tmp_val,
                    &tmp_len,
                    &tmp_expires_at
                );

            // Key exists in an SSTable, and isn't expired.
            if (sr == KV_LOOKUP_HIT) {
                exists = (tmp_expires_at == 0 || tmp_expires_at > now);
                free(tmp_val);
                break;
            }

            // A tombstone means the key was already deleted, so older SSTables should not be checked.

            if (sr == KV_LOOKUP_TOMBSTONE) {
                break;
            }
        }
    }

    if (!exists) {
        pthread_rwlock_unlock(&g_lock);
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

    pthread_rwlock_unlock(&g_lock);

    return 1;
}

void kv_store_destroy(void) {

    // Stop and join the sweep thread first- it must not still be
    // running when the memtable/WAL/SSTables below are freed.
    pthread_mutex_lock(&g_sweep_mutex);
    g_sweep_stop = 1;
    pthread_cond_signal(&g_sweep_cond);
    pthread_mutex_unlock(&g_sweep_mutex);

    if (g_sweep_running) {
        pthread_join(g_sweep_thread, NULL);
        g_sweep_running = 0;
    }

    // Lock before destroying shared state.
    pthread_rwlock_wrlock(&g_lock);

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

    pthread_rwlock_unlock(&g_lock);
}