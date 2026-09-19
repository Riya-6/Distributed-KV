#ifndef KV_NET_H
#define KV_NET_H

#include <stdint.h>
#include <netinet/in.h>

int net_listen(uint16_t port, int backlog);

int net_accept(int listen_fd, struct sockaddr_in *out_addr);

void net_close(int fd);

#endif 
