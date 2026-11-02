#pragma once
#ifdef __SWITCH__
#include <sys/_iovec.h>
#else
#include_next <sys/uio.h>
#endif
