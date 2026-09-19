#include "kv/server.h"

#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>

#include "kv/command.h"
#include "kv/net.h"
#include "kv/protocol.h"


#define KV_CONN_BUF_CAP (KV_HEADER_LEN + KV_MAX_PAYLOAD_LEN)

/*
 * Send all bytes in the buffer.
 * send() may send only part of the data, so keep calling it until everything is sent.
 */
static int send_all(int fd, const uint8_t *buf, size_t len) {
    size_t sent = 0;

    while (sent < len) {
        ssize_t n = send(fd, buf + sent, len - sent, 0);

        if (n < 0) {
            if (errno == EINTR) continue;

            return -1;
        }

        sent += (size_t)n;
    }

    return 0;
}

/*
 * Decide what response to send for each command.
 * Temporary stub, real storage logic will be added later.
 */
static void dispatch_stub(const kv_command_t *cmd, uint8_t *resp_opcode) {
    switch (cmd->type) {

    // PING gets a PONG response. 
    case KV_CMD_PING:
        *resp_opcode = KV_OP_PONG;
        break;

    // GET always returns NOT_FOUND for now.
    case KV_CMD_GET:
        *resp_opcode = KV_OP_NOT_FOUND;
        break;

    // SET and DELETE return OK for now.
    case KV_CMD_SET:
    case KV_CMD_DELETE:
        *resp_opcode = KV_OP_OK;
        break;
    }
}


int kv_handle_client(int client_fd) {

    // Allocate the connection buffer on the heap.
    
    uint8_t *buf = malloc(KV_CONN_BUF_CAP);

    if (buf == NULL) {
        return -1;
    }
    size_t buf_len = 0;
    int result = 0;

    // Keep handling requests from this client.
    for (;;) {

        /*
         * Process any complete frames already in the buffer.
         * This also allows multiple requests to be processed when they arrive together.
         */
        for (;;) {
            kv_frame_t frame;
            int prc = kv_parse_frame(buf, buf_len, &frame);

            if (prc == 0) {
                break;
            }

            if (prc == -1) {
                result = -1;
                goto done;
            }

            kv_command_t cmd;

            uint8_t resp_header[KV_HEADER_LEN];

            if (kv_decode_command(&frame, &cmd) < 0) {
                result = -1;
                goto done;
            }
            uint8_t resp_opcode = KV_OP_ERR;

            dispatch_stub(&cmd, &resp_opcode);

            /*
             * Create the response frame.
             * The current stub sends no payload.
             */
            size_t resp_len = kv_encode_response(
                resp_opcode,
                NULL,
                0,
                resp_header,
                sizeof(resp_header)
            );

            /*
             * Stop if encoding or sending fails.
             */
            if (resp_len == 0 ||
                send_all(client_fd, resp_header, resp_len) < 0) {

                result = -1;
                goto done;
            }

            // Calculate how many bytes are left in the buffer
        
            size_t remaining = buf_len - frame.frame_len;

            if (remaining > 0) {
                memmove(
                    buf,
                    buf + frame.frame_len,
                    remaining
                );
            }

            buf_len = remaining;
        }

        ssize_t n = recv(
            client_fd,
            buf + buf_len,
            KV_CONN_BUF_CAP - buf_len,
            0
        );

        if (n < 0) {
            // Retry if recv() was interrupted. 
            if (errno == EINTR) {
                continue;
            }

            result = -1;
            goto done;
        }

        // recv() returning 0 means the client disconnected.
         
        if (n == 0) {
            result = 0;
            goto done;
        }
        buf_len += (size_t)n;
    }

done:
    // Free the connection buffer before returning.
    free(buf);

    return result;
}

/*
 * Shared state between kv_run_server() (one thread) and
 * kv_stop_server()/kv_server_port() (called from another thread). 
 * Guarded by g_state_mutex throughout.
 */
static pthread_mutex_t g_state_mutex = PTHREAD_MUTEX_INITIALIZER;
static int g_listen_fd = -1;
static uint16_t g_port = 0;
static int g_stop_requested = 0;

// How often the accept loop wakes up on its own to check whether kv_stop_server() was called. 
 
#define KV_ACCEPT_POLL_MS 100

int kv_run_server(uint16_t port) {
    int listen_fd = net_listen(port, 16);
    if (listen_fd < 0) return -1;

    //Set the receive timeout for the listening socket 
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = KV_ACCEPT_POLL_MS * 1000;
    setsockopt(listen_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in bound_addr;
    socklen_t bound_len = sizeof(bound_addr);
    uint16_t bound_port = 0;
    if (getsockname(listen_fd, (struct sockaddr *)&bound_addr, &bound_len) == 0) {
        bound_port = ntohs(bound_addr.sin_port);
    }

    pthread_mutex_lock(&g_state_mutex);
    g_listen_fd = listen_fd;
    g_port = bound_port; 
    g_stop_requested = 0;
    pthread_mutex_unlock(&g_state_mutex);

    for (;;) {
        pthread_mutex_lock(&g_state_mutex);
        int stop = g_stop_requested;
        pthread_mutex_unlock(&g_state_mutex);
        if (stop) break;

        int client_fd = net_accept(listen_fd, NULL);
        if (client_fd < 0) {
        // If accept() failed, check if it was due to a timeout.
            continue;
        }

        kv_handle_client(client_fd);
        net_close(client_fd);
    }

    pthread_mutex_lock(&g_state_mutex);
    net_close(g_listen_fd);
    g_listen_fd = -1;
    g_port = 0;
    pthread_mutex_unlock(&g_state_mutex);

    return 0;
}

void kv_stop_server(void) {
    pthread_mutex_lock(&g_state_mutex);
    g_stop_requested = 1;
    pthread_mutex_unlock(&g_state_mutex);
}

uint16_t kv_server_port(void) {
    pthread_mutex_lock(&g_state_mutex);
    uint16_t port = g_port;
    pthread_mutex_unlock(&g_state_mutex);
    return port;
}