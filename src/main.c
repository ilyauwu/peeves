#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/fcntl.h>
#include <sys/socket.h>
#include <sys/unistd.h>

#include <mpscq.h>

#include "cmdline.h"
#include "foundation.h"
#include "logger.h"

#define DELAY_USEC (1000u)
#define RECONN_DELAY_USEC (10000u)

#define PACKET_HEADER_SIZE (4lu)

#define PACKET_DATA_SIZE_MIN (16lu)
#define PACKET_DATA_SIZE_MAX (128lu)

#define PACKET_QUEUE_CAP (1024lu)

typedef char PacketHeader[PACKET_HEADER_SIZE];
typedef char PacketData[PACKET_DATA_SIZE_MAX];

typedef struct Packet {
    union {
        uint32_t size;
        PacketHeader header;
    };
    PacketData data;
} Packet;

typedef struct Socket {
    int fd;
    struct sockaddr_in addr;
} Socket;

typedef struct Context {
    Socket in;
    Socket out;
    PacketHeader header;
    struct mpscq* queue;
} Context;

static volatile sig_atomic_t is_running = 1;

static Context* get_context() {
    static Context object = {
        .header = {0x43, 0x41, 0x4b, 0x45}
    };
    return &object;
}

static void shutdown_handler(int sig) {
    is_running = 0;
}

static Status sigaction_set() {
    struct sigaction action = {0};

    action.sa_handler = shutdown_handler;
    action.sa_flags = SA_RESETHAND;
    sigfillset(&action.sa_mask);

    const int sigs[] = {SIGINT, SIGTERM, SIGPIPE};
    for (size_t i = 0; i < sizeof(sigs) / sizeof(int); i++) {
        const int sig = sigs[i];
        const int err = sigaction(sigs[i], &action, NULL);
        if (err) {
            fprintf(stderr, "error: sigaction: %s\n", strerror(errno));
            return STATUS_ERR;
        }
    }

    action.sa_handler = SIG_IGN;
    action.sa_flags = 0x0;

    const int err = sigaction(SIGPIPE, &action, NULL);
    if (err) {
        fprintf(stderr, "error: sigaction: %s\n", strerror(errno));
        return STATUS_ERR;
    }

    return STATUS_OK;
}

static Status socket_create(Socket* s, const int type, const char* addr, const short port) {
    if (!s) {
        return STATUS_ERR;
    }

    s->fd = socket(AF_INET, type, 0);

    int err = s->fd == -1;
    if (err) {
        fprintf(stderr, "error: socket: %s\n", strerror(errno));
        return STATUS_SOCKET_ERR;
    }

    err = fcntl(s->fd, F_SETFL, fcntl(s->fd, F_GETFL) | O_NONBLOCK);
    if (err) {
        fprintf(stderr, "error: fcntl: %s\n", strerror(errno));
        close(s->fd);
        return STATUS_SOCKET_ERR;
    }

    if (s->addr.sin_family == AF_UNSPEC) {
        s->addr.sin_family = AF_INET;
        s->addr.sin_port = htons(port);

        err = inet_pton(AF_INET, addr, &s->addr.sin_addr.s_addr) <= 0;
        if (err) {
            fprintf(stderr, "error: inet_pton\n");
            close(s->fd);
            return STATUS_SOCKET_ERR;
        }
    }

    if (type == SOCK_STREAM) {
        err = setsockopt(s->fd, IPPROTO_TCP, TCP_NODELAY, &(int){1}, sizeof(int));
        if (err) {
            fprintf(stderr, "error: setsockopt: %s\n", strerror(errno));
            close(s->fd);
            return STATUS_SOCKET_ERR;
        }

        return STATUS_OK;
    }

    err = bind(s->fd, (struct sockaddr*)&s->addr, sizeof(struct sockaddr_in));
    if (err) {
        fprintf(stderr, "error: bind: %s\n", strerror(errno));
        close(s->fd);
        return STATUS_SOCKET_ERR;
    }

    return STATUS_OK;
}

static Status socket_destroy(Socket* s) {
    if (!s) {
        return STATUS_ERR;
    }

    const int fd = s->fd;

    s->fd = -1;

    const int err = close(fd);
    if (err) {
        fprintf(stderr, "error: close: %s\n", strerror(errno));
        return STATUS_SOCKET_ERR;
    }

    return STATUS_OK;
}

static Status send_packets(void* arg) {
    (void)arg;

    Context* context = get_context();
    Socket* s = &context->out;

    bool is_connected = false;

    Packet* packet = NULL;

    while(is_running) {
        if (!is_connected) {
            if (s->fd == -1) {
                socket_create(s, SOCK_STREAM, NULL, 0);
            }

            is_connected = connect(s->fd, (struct sockaddr*)&s->addr, sizeof(struct sockaddr_in)) == 0;
            if (!is_connected) {
                usleep(RECONN_DELAY_USEC);
                continue;
            }
        }

        if (!packet) {
            const bool is_dequeued = (packet = mpscq_dequeue(context->queue));
            if (!is_dequeued) {
                usleep(DELAY_USEC);
                continue;
            }
        }

        const size_t size = packet->size + sizeof(PacketHeader);
        memcpy(packet->header, context->header, sizeof(PacketHeader));

        const int sent = send(s->fd, packet, size, 0);
        if (sent == -1) {
            if (errno == ECONNRESET || errno == EPIPE) {
                is_connected = false;
                socket_destroy(s);
            }
            fprintf(stderr, "error: send: %s\n", strerror(errno));
        } else {
            LOG("[INFO] send packet: %s", packet->header);
            free(packet);
            packet = NULL;
        }
    }

    if (packet) {
        free(packet);
    }

    return STATUS_OK;
}

static Status recv_packets() {
    const Context* context = get_context();

    Packet* packet = NULL;

    while(is_running) {
        if (!packet) {
            packet = calloc(1, sizeof(Packet));
        }

        packet->size = recv(context->in.fd, packet->data, PACKET_DATA_SIZE_MAX, 0);
        if (packet->size == -1) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                fprintf(stderr, "error: recv: %s\n", strerror(errno));
            }
        } else if (packet->size >= PACKET_DATA_SIZE_MIN) {
            const bool is_enqueued = mpscq_enqueue(context->queue, packet);
            if (is_enqueued) {
                LOG("[INFO] recv packet: %s", packet->data);
                packet = NULL;
                continue;
            }
            memset(packet, 0x0, packet->size);
        }

        usleep(DELAY_USEC);
    }

    if (packet) {
        free(packet);
    }

    return STATUS_OK;
}

static void shutdown_gracefully() {
    Context* contex = get_context();

    if (contex->queue) {
        mpscq_destroy(contex->queue);
        contex->queue = NULL;
    }

    if (contex->out.fd != -1) {
        socket_destroy(&contex->out);
    }

    if (contex->in.fd != -1) {
        socket_destroy(&contex->in);
    }

#ifdef LOG_ENABLED
    logger_shutdown();
#endif
}

int main(int argc, char** argv) {
    Status status = sigaction_set();
    if (status != STATUS_OK) {
        return status;
    }

    struct gengetopt_args_info args_info = {0};
    cmdline_parser_init(&args_info);

    int err = cmdline_parser(argc, argv, &args_info);
    if (err) {
        cmdline_parser_free(&args_info);
        return STATUS_CMDLINE_ERR;
    }

    Context* context = get_context();

    status = socket_create(&context->in, SOCK_DGRAM, args_info.in_addr_arg, args_info.in_port_arg);
    if (status != STATUS_OK) {
        cmdline_parser_free(&args_info);
        goto gracefull_shutdown;
    }

    status = socket_create(&context->out, SOCK_STREAM, args_info.out_addr_arg, args_info.out_port_arg);
    if (status != STATUS_OK) {
        cmdline_parser_free(&args_info);
        goto gracefull_shutdown;
    }

    memcpy(context->header, args_info.header_arg, MIN(sizeof(PacketHeader), strlen(args_info.header_arg)));

#ifdef LOG_ENABLED
    if (args_info.log_given) {
        status = logger_init(args_info.log_arg);
        if (status != STATUS_OK) {
            cmdline_parser_free(&args_info);
            goto gracefull_shutdown;
        }
    }
#endif

    cmdline_parser_free(&args_info);

    context->queue = mpscq_create(NULL, PACKET_QUEUE_CAP);
    if (!context->queue) {
        fprintf(stderr, "error: mpscq_create\n");
        status = STATUS_ERR;
        goto gracefull_shutdown;
    }

    thrd_t sender = {0};
    if (thrd_create(&sender, send_packets, NULL) != thrd_success) {
        fprintf(stderr, "error: thrd_create\n");
        status = STATUS_THRD_ERR;
        goto gracefull_shutdown;
    }

    recv_packets();

    if (thrd_join(sender, NULL) != thrd_success) {
        fprintf(stderr, "error: thrd_join: %lu\n", sender);
        status = STATUS_THRD_ERR;
    }

gracefull_shutdown:
    shutdown_gracefully();

    return status;
}
