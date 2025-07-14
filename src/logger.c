#include "logger.h"

#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>

#include <sys/unistd.h>

#include <mpscq.h>

#include "foundation.h"

#define DELAY_USEC (500000u)

#define LOGGER_QUEUE_CAP (512lu)

typedef struct {
    FILE* file;
    char file_name[PATH_MAX + 1];

    thrd_t writer;
    atomic_bool is_writer_running;

    struct mpscq* queue;

    bool is_init;
} Logger;

static Logger* get_logger() {
    static Logger object = {0};
    return &object;
}

static int write_to_file(void* arg) {
    Logger* logger = get_logger();

    while(atomic_load_explicit(&logger->is_writer_running, memory_order_relaxed)) {
        const char* str = mpscq_dequeue(logger->queue);
        if (str) {
            fwrite(str, strlen(str), 1, logger->file);
            continue;
        }

        fflush(logger->file);

        usleep(DELAY_USEC);
    }

    return 0;
}


Status logger_init(const char* file_name) {
    if (!file_name) {
        return STATUS_ERR;
    }

    Logger* logger = get_logger();
    if (logger->is_init) {
        return STATUS_OK;
    }

    logger->file = fopen(file_name, "w");
    if (!logger->file) {
        fprintf(stderr, "error: fopen: %s\n", strerror(errno));
        return STATUS_ERR;
    }

    memcpy(logger->file_name, file_name, MIN(strlen(file_name), PATH_MAX));

    logger->queue = mpscq_create(NULL, LOGGER_QUEUE_CAP);
    if (!logger->queue) {
        fprintf(stderr, "error: mpscq_create\n");
        fclose(logger->file);
        return STATUS_ERR;
    }

    atomic_store(&logger->is_writer_running, true);

    const int ret = thrd_create(&logger->writer, write_to_file, NULL);
    if (ret != thrd_success) {
        fprintf(stderr, "error: thrd_create: %d\n", ret);
        mpscq_destroy(logger->queue);
        fclose(logger->file);
        return STATUS_ERR;
    }

    logger->is_init = true;

    return STATUS_OK;
}

Status logger_shutdown() {
    Logger* logger = get_logger();

    if (!logger->is_init) {
        return STATUS_OK;
    }

    atomic_store(&logger->is_writer_running, false);

    Status status = STATUS_OK;

    if (thrd_join(logger->writer, NULL) != thrd_success) {
        fprintf(stderr, "error: thrd_join: %lu\n", logger->writer);
        status = STATUS_THRD_ERR;
    }

    mpscq_destroy(logger->queue);
    fclose(logger->file);

    logger->is_init = false;

    return status;
}


void logger_log(const char *fmt, ...) {
    Logger* logger = get_logger();

    const bool is_ok = logger->is_init && fmt;
    if (!is_ok) {
        return;
    }

    va_list args_0 = {0};
    va_start(args_0, fmt);

    va_list args_1 = {0};
    va_copy(args_1, args_0);

    const size_t len = vsnprintf(NULL, 0, fmt, args_1) + 1;
    if (len > 0) {
        char* str = calloc(1, len);
        vsnprintf(str, len, fmt, args_0);
        mpscq_enqueue(logger->queue, str);
    }

    va_end(args_0);
    va_end(args_1);
}
