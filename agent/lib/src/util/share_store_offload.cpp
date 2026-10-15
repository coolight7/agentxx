#include "agentxx/util/share_store_offload.h"

#include "agentxx/agent/config.h"
#include "agentxx/agent/context.h"
#include "agentxx/middlewares/middleware.h"
#include "fmt/format.h"
#include "utilxx_base/log.h"
#include "utilxx_base/string_util.h"
#include <exception>

namespace agentxx {
namespace util {

ShareStoreOffload offloadShareStoreIfOversized(
    const std::shared_ptr<agentxx::agent::AgentContext>& agentContext,
    std::string_view                                     sessionId,
    std::string_view                                     text,
    size_t                                               limitLength
) {
    auto out = ShareStoreOffload{.text = std::string{text}};
    if (0 == limitLength || sessionId.empty() || !agentContext) {
        return out;
    }
    // 字节数不足阈值时不可能达到阈值字符数 (utf8 每字符至少 1 字节), 提前返回
    if (text.size() < limitLength) {
        return out;
    }

    // 按 utf8 字符数取截断片段:
    // - targetIndex = 第 limitLength 个字符之后的下标 (0 = 文本不足阈值字符数)
    // - lineCount = 片段内的换行数
    // - lastLineIndex = 片段内最后一个换行符的下标
    auto [targetIndex, lineCount, lastLineIndex]
        = utilxx_base::findIndexAndLastLineIndexByUtf8Length(text, limitLength);
    if (0 == targetIndex) {
        return out;
    }

    // share store 目标: 与 `agentxx_share_store` 工具同一套解析 —— 配置了
    // sharedShareStoreContext (同上下文子代理) 时读写父会话的 store, 保证这里写入的
    // id 与模型取回时读的是同一份内容
    auto shareStore = agentContext->agentConfig
                          ? agentContext->agentConfig->sharedShareStoreContext
                          : nullptr;
    if (!shareStore) {
        shareStore = agentContext->middlewareHandleContext;
    }
    if (!shareStore) {
        return out;
    }

    // 达到阈值: 原文存入 share store (id 按会话自增分配), 上下文里只保留片段
    // - 存储失败 (落库异常等) 按未卸载处理: 分页只是省 token 的优化, 不能让一条
    //   过长输入把整个轮次打断 (调用方拿到原文继续用)
    try {
        out.storeId   = shareStore->addShareStoreItemValue(sessionId, text);
        out.offloaded = true;

        const auto totalLineCount = utilxx_base::countLines(text);
        if (lastLineIndex >= targetIndex / 3) {
            // 按行截断后仍剩得下内容: 给出隐藏行号区间, 便于模型按行分页取回
            out.text = fmt::format(
                R"([Content offloaded. Use the `agentxx_share_store` tool to fetch the content by ID {}. Total {} lines, show [1, {}], hide [{}, {}].]
{}
...)",
                out.storeId,
                totalLineCount,
                lineCount,
                lineCount + 1,
                totalLineCount,
                text.substr(0, lastLineIndex)
            );
        } else {
            // 内容集中在少数行 (例如只有一行但特别长): 按行截断几乎不剩内容,
            // 按字符截断并让模型用 id 取全文
            out.text = fmt::format(
                R"([Content offloaded. Use the `agentxx_share_store` tool to fetch the full content by ID {}. Total {} lines.]
{}...)",
                out.storeId,
                totalLineCount,
                text.substr(0, targetIndex)
            );
        }
    } catch (const std::exception& e) {
        XX_LOGW("Share store offload failed ({} bytes): {}", text.size(), e.what());
        return ShareStoreOffload{.text = std::string{text}};
    }
    return out;
}

} // namespace util
} // namespace agentxx
