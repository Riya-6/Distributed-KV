/* Exposes usleep() from <unistd.h> under -std=c11 -- see Phase 1's
 * stage 4/5 test files for why. Must precede all includes. */
#define _DEFAULT_SOURCE

#include "kv/server.h"

#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "kv/command.h"
#include "kv/net.h"
#include "kv/protocol.h"
#include "kv/store.h"

#define KV_CONN_BUF_CAP (KV_HEADER_LEN + KV_MAX_PAYLOAD_LEN)

// Send all bytes in the buffer. 
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

// Execute the command and decide what response to send. 
static void dispatch(const kv_command_t *cmd, uint8_t *resp_opcode,
                      uint8_t **owned_payload, uint32_t *owned_payload_len) {
    *owned_payload = NULL;
    *owned_payload_len = 0;

    switch (cmd->type) {
    case KV_CMD_PING:
        *resp_opcode = KV_OP_PONG;
        break;

    case KV_CMD_GET: {
        uint8_t *value = NULL;
        uint32_t value_len = 0;

        int rc = kv_store_get(cmd->key, cmd->key_len, &value, &value_len);

        if (rc == 1) {
            *resp_opcode = KV_OP_VALUE;
            *owned_payload = value;
            *owned_payload_len = value_len;
        } else if (rc == 0) {
            *resp_opcode = KV_OP_NOT_FOUND;
        } else {
            *resp_opcode = KV_OP_ERR;
        }
        break;
    }

    case KV_CMD_SET: {
        int rc = kv_store_set(
            cmd->key,
            cmd->key_len,
            cmd->value,
            cmd->value_len
        );

        *resp_opcode = (rc == 0) ? KV_OP_OK : KV_OP_ERR;
        break;
    }

    case KV_CMD_DELETE: {
        int rc = kv_store_delete(cmd->key, cmd->key_len);

        *resp_opcode = (rc == 1) ? KV_OP_OK : KV_OP_NOT_FOUND;
        break;
    }
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

        // Process all complete frames currently in the buffer.
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
            uint8_t *owned_payload = NULL;
            uint32_t owned_payload_len = 0;

            // Execute the command using the KV store.
            dispatch(
                &cmd,
                &resp_opcode,
                &owned_payload,
                &owned_payload_len
            );

            int send_failed = 0;

            // Responses without a payload only need the fixed header.
            if (owned_payload_len == 0) {
                size_t resp_len = kv_encode_response(
                    resp_opcode,
                    NULL,
                    0,
                    resp_header,
                    sizeof(resp_header)
                );

                if (resp_len == 0 ||
                    send_all(client_fd, resp_header, resp_len) < 0) {
                    send_failed = 1;
                }
            } else {
                // GET responses need a buffer large enough for the value.
                size_t resp_cap = KV_HEADER_LEN + owned_payload_len;
                uint8_t *resp_buf = malloc(resp_cap);

                if (resp_buf == NULL) {
                    send_failed = 1;
                } else {
                    size_t resp_len = kv_encode_response(
                        resp_opcode,
                        owned_payload,
                        owned_payload_len,
                        resp_buf,
                        resp_cap
                    );

                    if (resp_len == 0 ||
                        send_all(client_fd, resp_buf, resp_len) < 0) {
                        send_failed = 1;
                    }

                    free(resp_buf);
                }
            }

            // Free the GET value copy after sending the response.
            free(owned_payload);

            if (send_failed) {
                result = -1;
                goto done;
            }

            // Remove the processed frame and keep any remaining bytes.
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

        // Read more data from the client.
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
 * Phase 4, Stage 1: one detached thread per client connection.
 * The client fd is passed directly as the thread argument.
 */

static atomic_int g_active_clients = 0;

static void *client_thread(void *arg) {
    int client_fd = (int)(intptr_t)arg;

    // Mark this client as active.
    atomic_fetch_add(&g_active_clients, 1);

    // Handle this client.
    kv_handle_client(client_fd);

    // Close the client connection.
    net_close(client_fd);

    // Mark this client as finished.
    atomic_fetch_sub(&g_active_clients, 1);

    return NULL;
}

// Shared server state protected by the mutex.
static pthread_mutex_t g_state_mutex = PTHREAD_MUTEX_INITIALIZER;
static int g_listen_fd = -1;
static uint16_t g_port = 0;
static int g_stop_requested = 0;

// How often the server checks for shutdown.
#define KV_ACCEPT_POLL_MS 100

int kv_run_server(uint16_t port) {
    int listen_fd = net_listen(port, 16);

    if (listen_fd < 0) {
        return -1;
    }

    // Set a timeout so the loop can check the stop flag.
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = KV_ACCEPT_POLL_MS * 1000;

    setsockopt(
        listen_fd,
        SOL_SOCKET,
        SO_RCVTIMEO,
        &tv,
        sizeof(tv)
    );

    // Get the actual port used by the socket.
    struct sockaddr_in bound_addr;
    socklen_t bound_len = sizeof(bound_addr);
    uint16_t bound_port = 0;

    if (getsockname(
            listen_fd,
            (struct sockaddr *)&bound_addr,
            &bound_len
        ) == 0) {
        bound_port = ntohs(bound_addr.sin_port);
    }

    // Store the server state safely.
    pthread_mutex_lock(&g_state_mutex);

    g_listen_fd = listen_fd;
    g_port = bound_port;
    g_stop_requested = 0;

    pthread_mutex_unlock(&g_state_mutex);

    // Keep accepting clients until shutdown is requested.
    for (;;) {
        pthread_mutex_lock(&g_state_mutex);
        int stop = g_stop_requested;
        pthread_mutex_unlock(&g_state_mutex);

        if (stop) {
            break;
        }

        // Accept a new client.
        int client_fd = net_accept(listen_fd, NULL);

        if (client_fd < 0) {
            continue;
        }

        // Create a thread to handle the client.
        pthread_t client_th;

        if (pthread_create(
                &client_th,
                NULL,
                client_thread,
                (void *)(intptr_t)client_fd
            ) != 0) {

            // Handle the client directly if thread creation fails.
            kv_handle_client(client_fd);
            net_close(client_fd);
            continue;
        }

        // No need to join a detached thread.
        pthread_detach(client_th);
    }

    // Wait for active clients to finish before destroying the store.
    for (int waited_ms = 0;
         atomic_load(&g_active_clients) > 0 && waited_ms < 3000;
         waited_ms += 20) {

        usleep(20 * 1000);
    }

    // Close the listening socket and reset the server state.
    pthread_mutex_lock(&g_state_mutex);

    net_close(g_listen_fd);
    g_listen_fd = -1;
    g_port = 0;

    pthread_mutex_unlock(&g_state_mutex);

    // Destroy the KV store after clients have finished.
    kv_store_destroy();

    return 0;
}

void kv_stop_server(void) {
    pthread_mutex_lock(&g_state_mutex);

    // Request the server to stop accepting clients.
    g_stop_requested = 1;

    pthread_mutex_unlock(&g_state_mutex);
}

uint16_t kv_server_port(void) {
    pthread_mutex_lock(&g_state_mutex);

    // Read the current server port safely.
    uint16_t port = g_port;

    pthread_mutex_unlock(&g_state_mutex);

    return port;
}