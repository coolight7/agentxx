#pragma once

#include <string>

#if XX_IS_CLANG_D || XX_IS_GCC_D

#define XX_NO_SANITIZE_ADDRESS __attribute__((no_sanitize("address")))

#elif XX_IS_MSVC_D

// MSVC 的 __declspec(no_sanitize_address) 仅在启用 /fsanitize=address 时定义
#ifdef __SANITIZE_ADDRESS__
#define XX_NO_SANITIZE_ADDRESS __declspec(no_sanitize_address)
#else
#define XX_NO_SANITIZE_ADDRESS
#endif

#else

#define XX_NO_SANITIZE_ADDRESS // 其他编译器不做任何事

#endif

namespace agentxx {

namespace util {

[[nodiscard]] std::string getSystemName();

[[nodiscard]] bool isRunningInWSL();

}; // namespace util
}; // namespace agentxx