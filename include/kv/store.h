#ifndef KV_STORE_H
#define KV_STORE_H

#include <stddef.h>
#include <stdint.h>


int kv_store_open(const char *data_dir);

int kv_store_set(const uint8_t *key, uint16_t key_len, const uint8_t *value,
                  uint32_t value_len);

// Same as kv_store_set(), but ttl_seconds > 0 makes the key expire
// that many seconds from now (0 means never, same as kv_store_set()).
int kv_store_set_ttl(const uint8_t *key, uint16_t key_len, const uint8_t *value,
                      uint32_t value_len, uint32_t ttl_seconds);

int kv_store_get(const uint8_t *key, uint16_t key_len, uint8_t **out_value,
                  uint32_t *out_value_len);

int kv_store_delete(const uint8_t *key, uint16_t key_len);

void kv_store_destroy(void);

#endif 
