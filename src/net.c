
#include "kv/net.h"
#include <errno.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>


static void close_preserving_errno(int fd) {
    int saved = errno;
    close(fd);
    errno = saved;
}
//socket -> setsocketopt-> bind-> listen-> return fd
int net_listen(uint16_t port, int backlog) {
     // Create a TCP socket using IPv4.
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1; 
    }

    // Allow the server to reuse its local address when restarting after a previous connection.
    int one = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) < 0) {
        close_preserving_errno(fd);
        return -1;
    }
    //Create and initialize the server's address
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr)); 
    //Set IPv4
    addr.sin_family = AF_INET;
    // Listen on all available local IPv4 interfaces.
    addr.sin_addr.s_addr = htonl(INADDR_ANY); 
    // Set port number, convert it to network nyte order
    addr.sin_port = htons(port);              

    //Bind the socket to the IP address and port.
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close_preserving_errno(fd);
        return -1;
    }
    //Start listening
    if (listen(fd, backlog) < 0) {
        close_preserving_errno(fd);
        return -1;
    }

    return fd;
}

int net_accept(int listen_fd, struct sockaddr_in *out_addr) {
    // Store client address
    struct sockaddr_in peer;
    socklen_t peer_len = sizeof(peer);

    int client_fd;

    // Try accepting a client connection.
    do {
        client_fd = accept(listen_fd, (struct sockaddr *)&peer, &peer_len);
    } while (client_fd < 0 && errno == EINTR); // retry if a signal interrupted

    if (client_fd < 0) {
        return -1; 
    }

    if (out_addr != NULL) {
        *out_addr = peer;
    }
    return client_fd;
}

void net_close(int fd) {
    if (fd >= 0) {
        close(fd);
    }
}
