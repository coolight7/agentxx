#include "agentxx/middlewares/memory_file.h"

#include "agentxx/util/exception.h"
#include "asio/read.hpp"
#include "asio/redirect_error.hpp"
#include "asio/stream_file.hpp"
#include "asio/this_coro.hpp"
#include "asio/use_awaitable.hpp"
#include "fmt/format.h"
#include "utilxx_base/string_util.h"
#include "utilxx_base/system.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

namespace agentxx {
namespace middleware {

asio::awaitable<void>
    MemoryFileMiddlewareHandle::onAgentcallStartFunc(neograph::graph::NodeInput& in) {
    if (memoryFilePaths.empty()) {
        co_return;
    }
    // 首轮懒加载 / 插件运行期增删文件后的全量重读 (自愈缓存)
    if (false == haveLoaded || needReloadMemoryFiles) {
        haveLoaded            = true;
        needReloadMemoryFiles = false;

#if ASIO_HAS_FILE || BOOST_ASIO_HAS_FILE
        /// 文件异步 I/O 可用时取其 executor (可用的实际判断见 util 的
        /// isAsyncFileIoSupported, 编译期不支持的平台不会走到异步分支)
        auto currentIoCtx = co_await asio::this_coro::executor;
#endif

        fileContents.clear();
        std::string logContent;
        for (const auto& filepath : memoryFilePaths) {
            auto systemCharsetFilePath = utilxx_base::toCurrentSystemAbsolutePath(
                filepath,
                agentContext.lock()->getSessionWorkDir(in.ctx.thread_id)
            );
            co_await agentxx::util::catchErrorAsync<bool>(
                [&]() -> asio::awaitable<bool> {
                    std::string content;
#if ASIO_HAS_FILE || BOOST_ASIO_HAS_FILE
                    /// 文件异步 I/O 可用 (含运行时可真正使用 io_uring) 时异步加载,
                    /// 避免同步读盘阻塞 io_context 事件循环
                    if (utilxx_base::isAsyncFileIoSupported()) {
                        asio::stream_file          stream{currentIoCtx};
                        utilxx_base::AsioErrorCode errCode;
                        stream.open(systemCharsetFilePath, asio::stream_file::read_only, errCode);
                        if (false == stream.is_open()) {
                            logContent += fmt::format(
                                "┣━ ❌ Can not open Memory file: `{}` | {}\n",
                                filepath,
                                errCode.message()
                            );
                            co_return false;
                        }

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
                    } else
#endif
                    {
                        /// 同步阻塞读取文件 (兜底实现)
                        std::ifstream stream(systemCharsetFilePath);
                        if (!stream.is_open()) {
                            logContent
                                += fmt::format("┣━ ❌ Can not open Memory file: `{}`\n", filepath);
                            // NOTE: 此处位于协程 lambda 内 (不在外层 for 循环作用域),
                            // 打开失败与 asio 分支一致, 记录日志后直接结束本次加载
                            co_return false;
                        }
                        content.assign(
                            std::istreambuf_iterator<char>(stream),
                            std::istreambuf_iterator<char>()
                        );
                        stream.close();
                    }
                    utilxx_base::autoConvertToUtf8(content);
                    fileContents.emplace_back(filepath, content);
                    logContent += fmt::format("┣━ ✅ Loaded Memory file: `{}`\n", filepath);
                    co_return true;
                },
                [&](std::string errmsg) -> asio ::awaitable<bool> {
                    logContent += fmt::format(
                        "┣━ ❌ Load Memory file failed: `{}` | {}\n",
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

    // 缓存失效: 首次生成 / 资源纪元变更 (插件增删上下文文件) 时重建
    if (state->cachedResourceEpoch != resourceEpoch) {
        state->cachedResourceEpoch = resourceEpoch;
        state->cacheContextContent.clear();
        if (!fileContents.empty()) {
            std::ostringstream oss;
            oss << "\n## Memory Files\n\n";
            for (const auto& [filepath, content] : fileContents) {
                oss << fmt::format(
                    R"(<MemoryFile src="{}">
{}
</MemoryFile>
)",
                    filepath,
                    content
                );
            }
            state->cacheContextContent = oss.str();
        }
    }

    if (!state->cacheContextContent.empty()) {
        auto  agentCtxPtr = agentContext.lock();
        auto& appendSystemMsgList
            = agentCtxPtr->middlewareHandleContext->getGraphDataItemValue<std::vector<std::string>>(
                in.ctx.thread_id,
                agentxx::middleware::MiddlewareContext::graphDataKey_appendSystemMessage
            );
        appendSystemMsgList.push_back(state->cacheContextContent);
    }
    co_return;
}

void MemoryFileMiddlewareHandle::addMemoryFiles(std::vector<std::string> paths) {
    bool changed = false;
    for (auto& p : paths) {
        if (p.empty()) {
            continue;
        }
        // 去重: 与 yaml 主配置/已注册文件重复时不重复读取注入
        if (std::find(memoryFilePaths.begin(), memoryFilePaths.end(), p) == memoryFilePaths.end()) {
            memoryFilePaths.push_back(std::move(p));
            changed = true;
        }
    }
    if (!changed) {
        return;
    }
    ++resourceEpoch;
    needReloadMemoryFiles = haveLoaded;
}

void MemoryFileMiddlewareHandle::removeMemoryFiles(const std::vector<std::string>& paths) {
    bool changed = false;
    for (const auto& p : paths) {
        auto it = std::find(memoryFilePaths.begin(), memoryFilePaths.end(), p);
        if (it != memoryFilePaths.end()) {
            memoryFilePaths.erase(it);
            changed = true;
        }
    }
    if (!changed) {
        return;
    }
    ++resourceEpoch;
    needReloadMemoryFiles = haveLoaded;
}

} // namespace middleware
} // namespace agentxx
