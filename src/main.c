
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

#include "kv/server.h"
#include "kv/store.h"

static void *shutdown_signal_thread(void *arg) {
    sigset_t *set = (sigset_t *)arg;

    int caught_signal = 0;

    // Block here until SIGTERM or SIGINT arrives.
    sigwait(set, &caught_signal);

    // Ask the accept loop to stop; kv_run_server() returns once it does.
    kv_stop_server();

    return NULL;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s <data_dir> <port>\n", argv[0]);
        return 1;
    }

    const char *data_dir = argv[1];
    uint16_t port = (uint16_t)atoi(argv[2]);

    if (kv_store_open(data_dir) != 0) {
        fprintf(stderr, "kv_store_open failed for %s\n", data_dir);
        return 1;
    }

    // Block SIGTERM/SIGINT on this thread so sigwait() can catch them synchronously on the dedicated shutdown thread below.
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGTERM);
    sigaddset(&set, SIGINT);
    pthread_sigmask(SIG_BLOCK, &set, NULL);

    pthread_t shutdown_th;
    if (pthread_create(&shutdown_th, NULL, shutdown_signal_thread, &set) != 0) {
        fprintf(stderr, "failed to start shutdown signal thread\n");
        kv_store_destroy();
        return 1;
    }

    // Blocks until kv_stop_server() is called (from signal thread), then closes the listener and destroys the store itself.
    int rc = kv_run_server(port);

    pthread_join(shutdown_th, NULL);

    return (rc == 0) ? 0 : 1;
}
