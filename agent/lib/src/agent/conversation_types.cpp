#include "agentxx/agent/conversation_types.h"
#include <fmt/format.h>

namespace agentxx {
namespace agent {

static uint64_t fnv1a(const std::string& data, uint64_t seed) {
    uint64_t hash = seed;
    for (unsigned char c : data) {
        hash ^= c;
        hash *= 1099511628211ULL;
    }
    return hash;
}

void ChainHash::append(const std::string& serialized) {
    hash_ = fnv1a(serialized, hash_ == 0 ? 14695981039346656037ULL : hash_);
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
