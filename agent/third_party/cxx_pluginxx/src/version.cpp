#include "pluginxx/version.h"

#ifndef XX_VERSION_STRING
#define XX_VERSION_STRING "0.1.0"
#endif

namespace pluginxx {

std::string_view version() noexcept {
    return std::string_view{XX_VERSION_STRING};
}

} // namespace pluginxx
