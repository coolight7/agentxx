#include "agentxx/nodes/warp_handle.h"

namespace agentxx {
namespace nodes {

WarpBaseNodeInterface::WarpBaseNodeInterface(std::string_view name) :
    name_(name) {}

asio::awaitable<neograph::graph::NodeOutput>
    WarpBaseNodeInterface::run(neograph::graph::NodeInput in) {
    co_return neograph::graph::NodeOutput{};
}

std::string WarpBaseNodeInterface::get_name() const {
    return name_;
}

} // namespace nodes
} // namespace agentxx
