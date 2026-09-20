#pragma once

#include "panic.h"

#define STR_(x) #x
#define STR(x) STR_(x)

#define assert(cond)                                                        \
    do {                                                                    \
        if (!(cond))                                                        \
            kpanic(__FILE__ ":" STR(__LINE__) ": assert failed: " #cond);   \
    } while (0)
