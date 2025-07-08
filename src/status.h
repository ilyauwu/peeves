#pragma once

#include <limits.h>

enum Status : int {
    status_ok,
    status_error = INT_MIN,
    status_cmdline_error,
    status_socket_error,
    status_thrd_error,
};

typedef int Status;
