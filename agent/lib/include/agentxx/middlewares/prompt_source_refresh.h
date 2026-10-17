#pragma once

#include "agentxx/middlewares/middleware.h"

namespace agentxx {
namespace middleware {

/// 提示词来源刷新入口
///
/// **这是运行时提示词段落唯一的刷新入口** (model call 的 start 钩子):
/// - 首次读取 (Initial)
/// - 压缩完成后的刷新 (AfterCompaction)
/// - 资源变更后的刷新 (ResourcesChanged: 插件增删记忆文件/技能目录)
/// - 外部显式请求 (Explicit)
///
/// 不在这里做的事 (改动前先读 `agentxx/agent/prompt_sources.h` 的说明):
/// - 不监听文件变化、不轮询、不按轮次 stat 后自动刷新
/// - `agentcall start` 不读写段落
///
/// 代价: 没有 dirty 标记时只有一次布尔判断, 不产生任何 I/O; 有标记时按来源取文本,
/// 内容未变则不动段落 (不会制造 system 前缀变化)。将来插件侧的提示词读写接口也
/// 落在这个位置。
class PromptSourceRefreshHandle : public BaseMiddlewareHandle<BaseMiddlewareState> {
public:

    explicit PromptSourceRefreshHandle(std::weak_ptr<agentxx::agent::AgentContext> in_agentContext);

    asio::awaitable<void> onModelcallStartFunc(neograph::graph::NodeInput& in) override;
};

} // namespace middleware
} // namespace agentxx
