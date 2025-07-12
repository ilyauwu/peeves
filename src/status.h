#pragma once

#include <limits.h>

enum Status {
    STATUS_OK,
    STATUS_ERR = INT_MIN,
    STATUS_CMDLINE_ERR,
    STATUS_SOCKET_ERR,
    STATUS_THRD_ERR
};

typedef int Status;
