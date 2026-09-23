#ifndef KV_SSTABLE_H
#define KV_SSTABLE_H

#include <stddef.h>
#include <stdint.h>

#include "kv/memtable.h"


int kv_sstable_write(const char *path, const kv_memtable_t *mt);


typedef struct kv_sstable kv_sstable_t;


kv_sstable_t *kv_sstable_open(const char *path);


void kv_sstable_close(kv_sstable_t *sst);


kv_lookup_result_t kv_sstable_get(kv_sstable_t *sst, const uint8_t *key,
                                   uint16_t key_len, uint8_t **out_value,
                                   uint32_t *out_value_len);

size_t kv_sstable_count(const kv_sstable_t *sst);

int kv_sstable_entry_at(kv_sstable_t *sst, size_t index, kv_entry_view_t *out);


int kv_sstable_compact(const char **paths, size_t count, const char *out_path);

#endif 
