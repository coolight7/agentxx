/// pluginxx 宿主领域钩子 (通用表实现的取数入口)
///
/// 定位: 通用表 (**与会话/模型/工具/提示词/图无关**的表) 的实现位于 cxx_pluginxx,
/// 但其中少数入口需要宿主数据 (配置内容 / 语言 / 会话工作目录 / 会话取消状态 /
/// 插件清单 / 工作线程池 / 事件总线)。这些数据一律经本接口取, 因此 pluginxx 源码
/// 不包含任何宿主头、不依赖任何宿主类型。
///
/// 宿主侧用法: 宿主管理器实现本接口, 并在任何通用表入口可能被调用之前
/// 经 [PluginHostCore::setDomainHooks] 注入 (通常在管理器构造函数体内完成)。
///
/// 线程约定: 下列方法都在宿主 IO 线程被调用 (通用表入口先投递到 IO 线程),
/// 实现方无需额外加锁; [postToWorkerThread] 例外, 它把工作投递到工作线程池。
#pragma once

#include "pluginxx/host/event_bus.h"

#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace pluginxx {

/// 宿主领域钩子 (纯虚接口部分为必需项; 带默认实现的为可选项)
class DomainHooks {
public:

    virtual ~DomainHooks() = default;

    // ==================== 事件表 ====================

    /// 事件后端; 返回 nullptr 表示宿主不支持事件订阅/发布
    virtual std::shared_ptr<EventSource> eventSource() {
        return nullptr;
    }

    /// 补齐事件主题的命名空间 (订阅与发布都经它, 保证两侧主题一致)
    /// - agentxx 侧实现: 不以 `plugin.` / `client.` 开头时补 `plugin.` 前缀
    virtual std::string qualifyEventTopic(std::string_view topic) {
        return std::string{topic};
    }

    // ==================== 调度表 ====================

    /// 把一段阻塞工作投递到宿主工作线程池
    /// - `return` false 表示宿主不可用 (offload 以失败终结, 不静默丢工作)
    virtual bool postToWorkerThread(std::function<void()> fn) = 0;

    // ==================== config 表 ====================

    /// 宿主配置 JSON (`get_config` 的领域部分, 如 dataDir / projectRoot / platform)
    virtual std::string configJson() = 0;

    /// 指定工具的提示词配置 JSON (`get_tool_prompt`; 不存在返回空串)
    virtual std::string toolPromptJson(std::string_view toolName) = 0;

    /// 指定会话生效的工作目录 (`get_session_work_dir`; 空 sessionId = 默认工作目录)
    virtual std::string sessionWorkDir(std::string_view sessionId) = 0;

    /// 当前语言代码 (如 "en" / "zh-cn"); 空串由调用方回退为 "en"
    virtual std::string language() = 0;

    /// 指定语言 (返回值忽略: 只读查询失败与写入失败在 ABI 上都表现为空结果)
    virtual void setLanguage(std::string_view lang) = 0;

    // ==================== cancel 表 ====================

    /// 会话当前轮次是否已取消 (`is_cancelled`; 权威通知始终是取消回调)
    virtual bool isSessionCancelled(std::string_view sessionId) = 0;

    // ==================== plugins 表 ====================

    /// 全部插件 JSON 数组 (`list_plugins`)
    virtual std::string pluginsJson() = 0;

    /// 单个插件 JSON 对象 (`get_plugin` / `get_own_info`; 不存在返回空串)
    virtual std::string pluginJson(std::string_view name) = 0;
};

} // namespace pluginxx
