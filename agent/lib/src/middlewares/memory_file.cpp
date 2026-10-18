#include "agentxx/middlewares/memory_file.h"

#include "agentxx/agent/context.h"
#include "agentxx/util/exception.h"
#include "asio/read.hpp"
#include "asio/redirect_error.hpp"
#include "asio/stream_file.hpp"
#include "asio/this_coro.hpp"
#include "asio/use_awaitable.hpp"
#include "fmt/format.h"
#include "utilxx_base/hash.h"
#include "utilxx_base/string_util.h"
#include "utilxx_base/system.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

namespace agentxx {
namespace middleware {

namespace {

/// 段落模板: 每个记忆文件一段 (`<MemoryFile src="...">` 便于模型辨认来源)
constexpr std::string_view kSectionHeader = "\n## Memory Files\n\n";

/// 取文件时间戳 (mtime 计数 + 字节数); 失败返回 false
bool readFileStamp(const std::string& absolutePath, uint64_t& mtime, uint64_t& size) {
    std::error_code ec;
    auto            nativePath = utilxx_base::utf8ToPath(absolutePath);
    const auto      bytes      = std::filesystem::file_size(nativePath, ec);
    if (ec) {
        return false;
    }
    size = static_cast<uint64_t>(bytes);
    const auto writeTime = std::filesystem::last_write_time(nativePath, ec);
    if (ec) {
        return false;
    }
    mtime = static_cast<uint64_t>(writeTime.time_since_epoch().count());
    return true;
}

} // namespace

MemoryFileMiddlewareHandle::MemoryFileMiddlewareHandle(
    const std::vector<std::string>&             in_memoryFilePaths,
    std::weak_ptr<agentxx::agent::AgentContext> in_agentContext
) :
    BaseMiddlewareHandle<MemoryFileMiddlewareState>(
        "MemoryFileMiddlewareHandle",
        in_agentContext
    ),
    memoryFilePaths(in_memoryFilePaths) {
    auto ctx = in_agentContext.lock();
    if (!ctx || !ctx->promptSources) {
        XX_LOGW("[memory] no prompt source registry, memory files will not be injected");
        return;
    }
    promptSources_ = ctx->promptSources;

    // 登记段落来源: 相对路径按会话工作目录解析 → 文本随会话变化, perSession = true
    // (不同会话可以挂不同工作目录, 各存一份; 同一批路径只读一次)
    ctx->promptSources->registerSource(agentxx::agent::PromptSource{
        .key                 = std::string{kSectionKey},
        .owner               = std::string{kSectionOwner},
        .order               = 100,
        .slot                = agentxx::agent::PromptSectionSlot::Stable,
        .perSession          = true,
        .refreshOnCompaction = true,
        .timeout             = std::chrono::milliseconds{3000},
        .build =
            [this](const agentxx::agent::PromptRefreshContext& rc)
            -> asio::awaitable<std::optional<std::string>> {
            co_return co_await buildSectionAsync(rc);
        },
    });
}

MemoryFileMiddlewareHandle::~MemoryFileMiddlewareHandle() {
    // 撤销来源: 本对象销毁后 build 里捕获的 `this` 不再有效
    if (auto registry = promptSources_.lock()) {
        registry->removeSource(kSectionKey, kSectionOwner);
    }
}

asio::awaitable<std::optional<std::string>> MemoryFileMiddlewareHandle::readFileAsync(
    const std::string& absolutePath,
    std::string&       errorOut
) {
    std::string content;
    bool        ok = false;
    co_await agentxx::util::catchErrorAsync<bool>(
        [&]() -> asio::awaitable<bool> {
#if ASIO_HAS_FILE || BOOST_ASIO_HAS_FILE
            /// 文件异步 I/O 可用 (含运行时可真正使用 io_uring) 时异步加载,
            /// 避免同步读盘阻塞 io_context 事件循环
            if (utilxx_base::isAsyncFileIoSupported()) {
                auto                       ex = co_await asio::this_coro::executor;
                asio::stream_file          stream{ex};
                utilxx_base::AsioErrorCode errCode;
                stream.open(absolutePath, asio::stream_file::read_only, errCode);
                if (!stream.is_open()) {
                    errorOut = errCode.message();
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
                    errorOut = errCode.message();
                    co_return false;
                }
                ok = true;
                co_return true;
            }
#endif
            /// 同步阻塞读取文件 (兜底实现)
            std::ifstream stream(absolutePath);
            if (!stream.is_open()) {
                errorOut = "open failed";
                co_return false;
            }
            content.assign(
                std::istreambuf_iterator<char>(stream),
                std::istreambuf_iterator<char>()
            );
            stream.close();
            ok = true;
            co_return true;
        },
        [&](std::string errmsg) -> asio::awaitable<bool> {
            errorOut = std::move(errmsg);
            co_return false;
        }
    );
    if (!ok) {
        co_return std::nullopt;
    }
    utilxx_base::autoConvertToUtf8(content);
    co_return content;
}

asio::awaitable<std::optional<std::string>>
    MemoryFileMiddlewareHandle::buildSectionAsync(
        const agentxx::agent::PromptRefreshContext& ctx
    ) {
    if (memoryFilePaths.empty()) {
        // 没有配置任何记忆文件: 返回空串表示"该段落没有内容"
        // (与 nullopt 的区别: 空串会清空段落, 用于文件被全部摘除的场景)
        co_return std::string{};
    }

    // 1. 按会话工作目录把配置路径解析成绝对路径
    std::vector<std::string> absolutePaths;
    absolutePaths.reserve(memoryFilePaths.size());
    for (const auto& filepath : memoryFilePaths) {
        absolutePaths.push_back(
            utilxx_base::toCurrentSystemAbsolutePath(filepath, std::string{ctx.workDir})
        );
    }

    // 2. 路径集合指纹 → 该路径集合对应的缓存
    std::string joined;
    for (const auto& path : absolutePaths) {
        joined += path;
        joined.push_back('\n');
    }
    const uint64_t digest  = utilxx_base::hash::fnv1a64(joined);
    // 缓存按路径集合保存: 活跃会话数决定条目数; 超过上限时整体清空 (只是多读一次盘,
    // 不会产生错误的段落内容)
    if (cacheByPathSet_.size() > 64) {
        cacheByPathSet_.clear();
    }
    auto& cache   = cacheByPathSet_[digest];
    bool  changed = !cache.built;
    bool  anyOk   = false;

    oversizeFiles_.clear();
    std::string logContent;

    // 3. 逐文件比较时间戳: 全部未变 → 不读盘 (只返回上次文本)
    for (size_t i = 0; i < absolutePaths.size(); ++i) {
        const auto& configuredPath = memoryFilePaths[i];
        const auto& absolutePath   = absolutePaths[i];

        uint64_t mtime = 0;
        uint64_t size  = 0;
        if (!readFileStamp(absolutePath, mtime, size)) {
            logContent += fmt::format(
                "┣━ ❌ Can not stat Memory file: `{}`\n",
                configuredPath
            );
            // 保留该文件上次内容 (失败不影响其他文件)
            if (auto itOld = cache.files.find(absolutePath);
                itOld != cache.files.end() && itOld->second.ok) {
                anyOk = true;
            }
            continue;
        }

        auto& entry = cache.files[absolutePath];
        if (entry.ok && entry.stamp.mtime == mtime && entry.stamp.size == size) {
            anyOk = true; // 时间戳未变: 沿用上次内容, 本次不读盘
            continue;
        }

        changed = true;
        entry.stamp.mtime = mtime;
        entry.stamp.size  = size;

        std::string error;
        auto        content = co_await readFileAsync(absolutePath, error);
        if (!content.has_value()) {
            logContent += fmt::format(
                "┣━ ❌ Load Memory file failed: `{}` | {}\n",
                configuredPath,
                error
            );
            continue;
        }

        // 体积告警: 记忆文件整份注入系统提示词, 过大时持续占用上下文预算;
        // 只提示用户精简 (不截断、不改变注入方式)
        const size_t chars = utilxx_base::utf8GetLength(*content);
        if (chars > kOversizeWarnChars) {
            oversizeFiles_.emplace_back(configuredPath, chars);
            XX_LOGW(
                "Memory file too large: `{}` {} chars > {} chars; keep it short and "
                "index-like (list what exists and how to fetch it), detail should be "
                "read on demand",
                configuredPath,
                chars,
                kOversizeWarnChars
            );
            logContent += fmt::format(
                "┣━ ⚠️ Memory file too large: `{}` | {} chars (limit {})\n",
                configuredPath,
                chars,
                kOversizeWarnChars
            );
        }

        entry.content = std::move(*content);
        entry.ok      = true;
        anyOk         = true;
        logContent += fmt::format("┣━ ✅ Loaded Memory file: `{}`\n", configuredPath);
    }

    if (!logContent.empty()) {
        XX_LOGD(
            R"_(
┏━━━━━━ Memory File Load ━━━━━━┓
{}
┗━━━━━━ Memory File Load ━━━━━━┛
)_",
            logContent
        );
    }

    // 全部失败: 没有意见 (保留上次文本, 由注册表按失败计数)
    if (!anyOk) {
        co_return std::nullopt;
    }
    // 内容没有变化: 直接返回上次文本 (不制造任何提示词变化)
    if (cache.built && !changed) {
        co_return cache.text;
    }

    // 4. 重新拼接段落文本 (按配置顺序; 展示配置里的路径, 相对路径更短)
    std::ostringstream oss;
    oss << kSectionHeader;
    bool anySection = false;
    for (size_t i = 0; i < absolutePaths.size(); ++i) {
        const auto& absolutePath = absolutePaths[i];
        auto        itEntry      = cache.files.find(absolutePath);
        if (itEntry == cache.files.end() || !itEntry->second.ok) {
            continue;
        }
        oss << fmt::format(
            R"(<MemoryFile src="{}">
{}
</MemoryFile>
)",
            memoryFilePaths[i],
            itEntry->second.content
        );
        anySection = true;
    }
    if (!anySection) {
        co_return std::nullopt;
    }
    cache.text  = oss.str();
    cache.built = true;
    co_return cache.text;
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
    // 资源变更: 只标记, 实际读盘在下一个请求装配前 (model call 的 start 钩子)
    if (auto registry = promptSources_.lock()) {
        registry->markAllDirty(agentxx::agent::PromptRefreshReason::ResourcesChanged);
    }
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
    if (auto registry = promptSources_.lock()) {
        registry->markAllDirty(agentxx::agent::PromptRefreshReason::ResourcesChanged);
    }
}

} // namespace middleware
} // namespace agentxx
