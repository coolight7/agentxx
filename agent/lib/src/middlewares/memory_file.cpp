#include "agentxx/middlewares/memory_file.h"

#include "agentxx/util/exception.h"
#include "agentxx/util/string_util.h"
#include "asio/read.hpp"
#include "asio/redirect_error.hpp"
#include "asio/stream_file.hpp"
#include "asio/this_coro.hpp"
#include "asio/use_awaitable.hpp"
#include "fmt/format.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

namespace agentxx {
namespace middleware {

asio::awaitable<void>
    MemoryFileMiddlewareHandle::onAgentcallStartFunc(neograph::graph::NodeInput& in) {
    if (initMemoryFilePaths.empty()) {
        co_return;
    }
    if (false == haveLoaded) {
        haveLoaded = true;

#if ASIO_HAS_FILE || BOOST_ASIO_HAS_FILE
        auto currentIoCtx = co_await asio::this_coro::executor;
#endif

        fileContents.clear();
        std::string logContent;
        for (const auto& filepath : initMemoryFilePaths) {
            auto systemCharsetFilePath = filepath;
            agentxx::util::autoConvertToSystemPath(systemCharsetFilePath);
            co_await agentxx::util::catchErrorAsync<bool>(
                [&]() -> asio::awaitable<bool> {

#if ASIO_HAS_FILE || BOOST_ASIO_HAS_FILE
                    /// 异步加载文件, 避免同步读盘阻塞 io_context 事件循环
                    asio::stream_file        stream{currentIoCtx};
                    neograph_asio_error_code errCode;
                    stream.open(systemCharsetFilePath, asio::stream_file::read_only, errCode);
                    if (false == stream.is_open()) {
                        logContent += fmt::format(
                            "┣━ ❌ Can not open context file: `{}` | {}\n",
                            filepath,
                            errCode.message()
                        );
                        co_return false;
                    }

                    std::string content;
                    co_await asio::async_read(
                        stream,
                        asio::dynamic_buffer(content),
                        asio::transfer_all(),
                        asio::redirect_error(asio::use_awaitable, errCode)
                    );
                    stream.close();
                    if (errCode && errCode != asio::error::eof) {
                        throw std::system_error{errCode};
                    }
#else
                    /// 同步阻塞读取文件
                    std::ifstream stream(systemCharsetFilePath);
                    if (!stream.is_open()) {
                        logContent
                            += fmt::format("┣━ ❌ Can not open context file: `{}`\n", filepath);
                        continue;
                    }
                    auto content = std::string{
                        std::istreambuf_iterator<char>(stream),
                        std::istreambuf_iterator<char>()
                    };
                    stream.close();
#endif
                    fileContents.emplace_back(filepath, std::move(content));
                    logContent += fmt::format("┣━ ✅ Loaded context file: `{}`\n", filepath);
                    co_return true;
                },
                [&](std::string errmsg) -> asio ::awaitable<bool> {
                    logContent += fmt::format(
                        "┣━ ❌ Load context file failed: `{}` | {}\n",
                        filepath,
                        errmsg
                    );
                    co_return false;
                }
            );
        }

        XX_LOGD(
            R"_(
┏━━━━━━ Memory File Load ━━━━━━┓
{}
┗━━━━━━ Memory File Load ━━━━━━┛
)_",
            logContent
        );
    }

    // insert
    auto state = co_await getStateItem(in.ctx.thread_id);

    if (state->cacheContextContent.empty() && !fileContents.empty()) {
        std::ostringstream oss;
        oss << "\n## Memory Context Files\n\n";
        for (const auto& [filepath, content] : fileContents) {
            oss << fmt::format(
                R"(<MemoryContextFile src="{}">
{}
</MemoryContextFile>
)",
                filepath,
                content
            );
        }
        state->cacheContextContent = oss.str();
    }

    if (!state->cacheContextContent.empty()) {
        auto  agentCtxPtr = agentContext.lock();
        auto& appendSystemMsgList
            = agentCtxPtr->middlewareHandleContext->getGraphDataItemValue<std::vector<std::string>>(
                in.ctx.thread_id,
                agentxx::middleware::MiddlewareContext::graphDataKey_appendSystemMessage
            );
        auto size = appendSystemMsgList.size();
        appendSystemMsgList.push_back(state->cacheContextContent);
    }
    co_return;
}

} // namespace middleware
} // namespace agentxx
