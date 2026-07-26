#include "agentxx/nodes/wrap_handle.h"

namespace agentxx {
namespace nodes {

WrapBaseNodeInterface::WrapBaseNodeInterface(std::string_view name) :
    name_(name) {}

asio::awaitable<neograph::graph::NodeOutput>
    WrapBaseNodeInterface::run(neograph::graph::NodeInput in) {
    co_return neograph::graph::NodeOutput{};
}

std::string WrapBaseNodeInterface::get_name() const {
    return name_;
}

} // namespace nodes
} // namespace agentxx
