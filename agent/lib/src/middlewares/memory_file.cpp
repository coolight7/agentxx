#include "agentxx/middlewares/memory_file.h"

#include "fmt/format.h"
#include <filesystem>
#include <fstream>
#include <sstream>

namespace agentxx {
namespace middleware {

asio::awaitable<void>
    MemoryFileMiddlewareHandle::onAgentcallStartFunc(neograph::graph::NodeInput& in) {
    if (initMemoryFilePaths.empty()) {
        co_return;
    }
    if (haveLoaded) {
        co_return;
    }
    haveLoaded = true;

    fileContents.clear();
    std::string logContent;
    for (const auto& filepath : initMemoryFilePaths) {
        try {
            std::ifstream stream(filepath);
            if (!stream.is_open()) {
                logContent += fmt::format("┣━ ❌ Can not open context file: `{}`\n", filepath);
                continue;
            }
            auto content = std::string{
                std::istreambuf_iterator<char>(stream),
                std::istreambuf_iterator<char>()
            };
            stream.close();
            fileContents.emplace_back(filepath, std::move(content));
            logContent += fmt::format("┣━ ✅ Loaded context file: `{}`\n", filepath);
        } catch (const std::exception& e) {
            logContent
                += fmt::format("┣━ ❌ Load context file failed: `{}` | {}\n", filepath, e.what());
        }
    }

    XX_LOGD(
        R"_(
┏━━━━━━ Memory File Load ━━━━━━┓
{}
┗━━━━━━ Memory File Load ━━━━━━┛
)_",
        logContent
    );
    co_return;
}

asio::awaitable<void>
    MemoryFileMiddlewareHandle::onModelcallStartFunc(neograph::graph::NodeInput& in) {
    if (initMemoryFilePaths.empty()) {
        co_return;
    }

    auto state = co_await getStateItem(in.ctx.thread_id);

    if (state->cacheContextContent.empty() && !fileContents.empty()) {
        std::ostringstream oss;
        oss << "\n## Context Files\n\n";
        for (const auto& [filepath, content] : fileContents) {
            oss << fmt::format("### {}\n\n{}\n\n", filepath, content);
        }
        state->cacheContextContent = oss.str();
    }

    if (!state->cacheContextContent.empty()) {
        auto  agentCtxPtr = agentContext.lock();
        auto& appendSystemMsgList
            = agentCtxPtr->middlewareHandleContext->getGraphDataItemValue<std::vector<std::string>>(
                in.ctx.thread_id,
                agentxx::middleware::MiddlewareContext::graphDataKey_systemMessage
            );
        appendSystemMsgList.push_back(state->cacheContextContent);
    }
    co_return;
}

} // namespace middleware
} // namespace agentxx
