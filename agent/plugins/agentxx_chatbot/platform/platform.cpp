#include "platform.h"

#include "fake/fake_platform.h"
#include "qq/qq_platform.h"

namespace agentxx {
namespace chatbot {

std::unique_ptr<IPlatform> createPlatform(
    std::string_view     type,
    asio::any_io_executor ex,
    std::string_view     dataDir
) {
    if (type == "qq") {
        return std::make_unique<QqPlatform>(std::move(ex), std::string{dataDir});
    }
    if (type == "fake") {
        return std::make_unique<FakePlatform>(std::move(ex));
    }
    return nullptr;
}

} // namespace chatbot
} // namespace agentxx
