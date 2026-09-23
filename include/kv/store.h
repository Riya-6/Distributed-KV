#ifndef KV_STORE_H
#define KV_STORE_H

#include <stddef.h>
#include <stdint.h>


int kv_store_open(const char *data_dir);

int kv_store_set(const uint8_t *key, uint16_t key_len, const uint8_t *value,
                  uint32_t value_len);

int kv_store_get(const uint8_t *key, uint16_t key_len, uint8_t **out_value,
                  uint32_t *out_value_len);

int kv_store_delete(const uint8_t *key, uint16_t key_len);

void kv_store_destroy(void);

#endif 
