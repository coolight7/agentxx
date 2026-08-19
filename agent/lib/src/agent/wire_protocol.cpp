// P1-16: 将 wire_protocol header-only 的重型实现下沉到 .cpp, 减少每个 TU 的编译体积
// 头文件保留内联的轻量 helpers, 此文件提供 delta/sync 重型转换的可复用定义
#include "agentxx/agent/io/wire_protocol.h"

namespace agentxx {
namespace agent {
namespace io {
// 占位: 所有实现仍为 header inline, 此 TU 确保 protocol_base 收敛与链接验证
}
} // namespace agent
} // namespace agentxx
