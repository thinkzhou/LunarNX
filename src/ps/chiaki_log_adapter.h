#pragma once

#ifdef __SWITCH__

#include <chiaki/log.h>

namespace lunar::ps {

ChiakiLog makeChiakiDiagnosticLog(const char* name);

} // namespace lunar::ps

#endif
