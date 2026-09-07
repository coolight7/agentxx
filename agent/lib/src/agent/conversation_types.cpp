#include "agentxx/agent/conversation_types.h"
#include "agentxx/util/hash.h"
#include <fmt/format.h>

namespace agentxx {
namespace agent {

void ChainHash::append(std::string_view serialized) {
    // 用 count_ 判断首次追加, 而非 hash_==0 (合法链哈希也可能算出 0, 会错误重置种子)
    hash_ = agentxx::util::hash::fnv1a64(
        serialized,
        count_ == 0 ? agentxx::util::hash::kFnv1a64OffsetBasis : hash_
    );
    ++count_;
}

void ChainHash::reset() {
    hash_  = 0;
    count_ = 0;
}

uint64_t ChainHash::tail() const {
    return hash_;
}

uint64_t ChainHash::count() const {
    return count_;
}

std::string ChainHash::tailHex() const {
    return fmt::format("{:016x}", hash_);
}

} // namespace agent
} // namespace agentxx
