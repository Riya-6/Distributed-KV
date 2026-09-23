#ifndef KV_WAL_H
#define KV_WAL_H

#include <stddef.h>
#include <stdint.h>


typedef struct kv_wal kv_wal_t;

kv_wal_t *kv_wal_open(const char *path);

void kv_wal_close(kv_wal_t *wal);

int kv_wal_append_set(kv_wal_t *wal, const uint8_t *key, uint16_t key_len,
                       const uint8_t *value, uint32_t value_len);
int kv_wal_append_delete(kv_wal_t *wal, const uint8_t *key, uint16_t key_len);


int kv_wal_truncate(kv_wal_t *wal);


typedef void (*kv_wal_replay_set_fn)(const uint8_t *key, uint16_t key_len,
                                      const uint8_t *value, uint32_t value_len,
                                      void *ctx);
typedef void (*kv_wal_replay_delete_fn)(const uint8_t *key, uint16_t key_len,
                                         void *ctx);


int kv_wal_replay(const char *path, kv_wal_replay_set_fn on_set,
                   kv_wal_replay_delete_fn on_delete, void *ctx);

#endif 
