#ifndef KV_SERVER_H
#define KV_SERVER_H

#include <stdint.h>

int kv_handle_client(int client_fd);

int kv_run_server(uint16_t port);

void kv_stop_server(void);

uint16_t kv_server_port(void);

#endif 
