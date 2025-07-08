#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <liblfds600.h>

#include "cmdline.h"
#include "status.h"

#define MAX(A, B) ((A) > (B) ? (A) : (B))
#define MIN(A, B) ((A) < (B) ? (A) : (B))

#define SLEEP_TIME_USEC (250'000l)

#define PACKET_HEADER_SIZE (4lu)

#define PACKET_DATA_SIZE_MIN (16lu)
#define PACKET_DATA_SIZE_MAX (128lu)

#define PACKET_QUEUE_SIZE (128lu)

typedef char PacketHeader[PACKET_HEADER_SIZE];
typedef char PacketData[PACKET_DATA_SIZE_MAX];

typedef struct Packet {
    union {
        uint32_t size;
        PacketHeader header;
    };
    PacketData data;
} Packet;

static_assert(sizeof(Packet) == sizeof(PacketData) + sizeof(PacketHeader));

typedef struct Socket {
    int fd;
    struct sockaddr_in addr;
} Socket;

typedef struct Context {
    Socket in;
    Socket out;
    PacketHeader header;
    struct lfds600_queue_state* queue;
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
    struct sigaction action = {};

    action.sa_handler = shutdown_handler;
    action.sa_flags = SA_RESETHAND;
    sigfillset(&action.sa_mask);

    const int sigs[] = {SIGINT, SIGTERM, SIGPIPE};
    for (size_t i = 0; i < sizeof(sigs) / sizeof(int); i++) {
        const int sig = sigs[i];
        const int err = sigaction(sigs[i], &action, nullptr);
        if (err) {
            fprintf(stderr, "error: sigaction: %s\n", strerror(errno));
            return status_error;
        }
    }

    action.sa_handler = SIG_IGN;
    action.sa_flags = 0x0;

    const int err = sigaction(SIGPIPE, &action, nullptr);
    if (err) {
        fprintf(stderr, "error: sigaction: %s\n", strerror(errno));
        return status_error;
    }

    return status_ok;
}

static Status socket_create(Socket* s, const int type, const char* addr, const short port) {
    if (!s) {
        return status_error;
    }

    s->fd = socket(AF_INET, type, 0);

    int err = s->fd == -1;
    if (err) {
        fprintf(stderr, "error: socket: %s\n", strerror(errno));
        return status_socket_error;
    }

    err = fcntl(s->fd, F_SETFL, fcntl(s->fd, F_GETFL) | O_NONBLOCK);
    if (err) {
        fprintf(stderr, "error: fcntl: %s\n", strerror(errno));
        return status_socket_error;
    }

    if (s->addr.sin_family == AF_UNSPEC) {
        s->addr.sin_family = AF_INET;
        s->addr.sin_port = htons(port);

        err = inet_pton(AF_INET, addr, &s->addr.sin_addr.s_addr) <= 0;
        if (err) {
            fprintf(stderr, "error: inet_pton\n");
            return status_socket_error;
        }
    }

    if (type == SOCK_STREAM) {
        return status_ok;
    }

    err = bind(s->fd, (struct sockaddr*)&s->addr, sizeof(struct sockaddr_in));
    if (err) {
        fprintf(stderr, "error: bind: %s\n", strerror(errno));
        return status_socket_error;
    }

    return status_ok;
}

static Status socket_destroy(Socket* s) {
    if (!s) {
        return status_error;
    }

    const int fd = s->fd;

    s->fd = -1;

    const int err = close(fd);
    if (err) {
        fprintf(stderr, "error: close: %s\n", strerror(errno));
        return status_socket_error;
    }

    return status_ok;
}

static Status send_packets(void*) {
    Context* context = get_context();
    Socket* s = &context->out;

    bool is_connected = false;

    Packet* packet = nullptr;

    while(is_running) {
        if (!is_connected) {
            if (s->fd == -1) {
                socket_create(s, SOCK_STREAM, nullptr, 0);
            }

            is_connected = connect(s->fd, (struct sockaddr*)&s->addr, sizeof(struct sockaddr_in)) == 0;
            if (!is_connected) {
                // fprintf(stderr, "error: connect: %s\n", strerror(errno));
                usleep(SLEEP_TIME_USEC);
                continue;
            }
        }

        if (!packet) {
            lfds600_queue_dequeue(context->queue, (void**)&packet);
        }

        if (packet) {
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
                free(packet);
                packet = nullptr;
            }
        }

        usleep(SLEEP_TIME_USEC);
    }

    return status_ok;
}

static Status recv_packets() {
    const Context* context = get_context();

    Packet* packet = nullptr;

    while(is_running) {
        if (!packet) {
            packet = calloc(1, sizeof(Packet));
        }

        packet->size = recv(context->in.fd, packet->data, PACKET_DATA_SIZE_MAX, 0);
        if (packet->size == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // TODO: maybe poll
            } else {
                fprintf(stderr, "error: recv: %s\n", strerror(errno));
            }
        } else if (packet->size >= PACKET_DATA_SIZE_MIN) {
            if (lfds600_queue_enqueue(context->queue, packet)) {
                packet = nullptr;
            } else {
                memset(packet, 0x0, sizeof(Packet));
            }
        }

        usleep(SLEEP_TIME_USEC);
    }

    if (packet) {
        free(packet);
    }

    return status_ok;
}

int main(int argc, char** argv) {
    Status status = sigaction_set();
    if (status != status_ok) {
        return status;
    }

    struct gengetopt_args_info args_info = {};
    cmdline_parser_init(&args_info);

    int err = cmdline_parser(argc, argv, &args_info);
    if (err) {
        return status_cmdline_error;
    }

    Context* context = get_context();

    status = socket_create(&context->in, SOCK_DGRAM, args_info.in_addr_arg, args_info.in_port_arg);
    if (status != status_ok) {
        cmdline_parser_free(&args_info);

        return status;
    }

    status = socket_create(&context->out, SOCK_STREAM, args_info.out_addr_arg, args_info.out_port_arg);
    if (status != status_ok) {
        socket_destroy(&context->in);

        cmdline_parser_free(&args_info);

        return status;
    }

    memcpy(context->header, args_info.header_arg, MIN(sizeof(PacketHeader), strlen(args_info.header_arg)));

    cmdline_parser_free(&args_info);

    if (!lfds600_queue_new(&context->queue, PACKET_QUEUE_SIZE)) {
        fprintf(stderr, "error: lfds600_queue_new\n");

        socket_destroy(&context->out);
        socket_destroy(&context->in);

        return status_error;
    }

    thrd_t sender = {};
    if (thrd_create(&sender, send_packets, nullptr) != thrd_success) {
        fprintf(stderr, "error: thrd_create\n");

        lfds600_queue_delete(context->queue, nullptr, nullptr);

        socket_destroy(&context->out);
        socket_destroy(&context->in);

        return status_thrd_error;
    }

    recv_packets();

    if (thrd_join(sender, nullptr) != thrd_success) {
        fprintf(stderr, "error: thrd_join\n");
        status = status_thrd_error;
    }

    lfds600_queue_delete(context->queue, nullptr, nullptr);

    socket_destroy(&context->out);
    socket_destroy(&context->in);

    return status;
}
