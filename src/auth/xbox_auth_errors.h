#pragma once

#include <string>

namespace lunar::auth {

std::string describeXstsFailure(int status, const std::string& body);

} // namespace lunar::auth
