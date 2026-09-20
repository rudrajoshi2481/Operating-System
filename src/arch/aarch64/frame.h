#pragma once

#include <stdint.h>

struct trap_frame {
    uint64_t x[31];
    uint64_t elr;
    uint64_t spsr;
};
