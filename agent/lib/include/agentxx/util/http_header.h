#pragma once

#include "agentxx/util/string_util.h"

namespace agentxx {
namespace util {

class HeaderMap {
public:

    using _BaseMap = agentxx::util::IgnoreCaseMap<std::vector<std::string>>;
    _BaseMap data;

    HeaderMap() = default;
    HeaderMap(_BaseMap in_data);

    bool empty() const;

    bool contains(std::string_view name) const noexcept;

    _BaseMap::iterator get(std::string_view name);

    std::string_view getSingle(std::string_view name) const noexcept;

    void set(std::string_view name, const std::vector<std::string>& value);

    void set(std::string_view name, std::string_view value);
};

} // namespace util
} // namespace agentxx