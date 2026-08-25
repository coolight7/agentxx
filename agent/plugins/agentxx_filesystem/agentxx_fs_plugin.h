// agentxx_filesystem 插件 —— 共享头
// - 从 libagentxx src/tools/filesystem 拆分独立 (同名同行为):
//     agentxx_filesystem_list / read / write / edit / glob / grep
// - 插件不链接 libagentxx: 日志经宿主 agentxx.agent.log 接口表转发 (替代 XX_LOG);
//   业务逻辑在 filesystem_impl.h (纯函数, 亦供测试直接调用)
// - g_host 由入口 (agentxx_filesystem.cpp) 在 entry 时装配
#pragma once

#include "agentxx/plugin/plugin_api.h"
#include "agentxx/plugin/plugin_guard.h"
#include "agentxx/plugin/plugin_iface_helper.h"
#include "agentxx/plugin/plugin_tool_sync.h"
#include <fmt/format.h>
#include <neograph/json.h>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace agentxx_fs_plugin {

// =====================================================================
// 多实例约定 (2026-08 API v1): 同一动态库可被同一进程内不同 agent 宿主
// 各自加载为独立实例, 本插件不持有任何可变全局; 功能状态一律存于每实例
// PluginCtx (create 创建经 *plugin_ctx 交付宿主, 工具回调经 spec.user_data
// 恢复); 日志经显式传入的 ctx 路由到宿主接口表。
// =====================================================================

/// 每实例上下文 (多实例安全的功能状态载体)
struct PluginCtx {
    const AgentxxHost*           host    = nullptr;
    agentxx::plugin::AgentIfaces iface   {};
    /// spec 字符串稳定存储 (每实例独立, 卸载随 ctx 释放)
    std::vector<std::string>     storage {};
    /// 同步垫片适配器存储 (每注册工具一个, 随实例销毁释放; unique_ptr 目标
    /// 地址稳定 —— 注册后三件套回调引用其内容, 容器扩容不失效)
    std::vector<std::unique_ptr<AgentxxSyncToolShim>> sync_tool_shims;
    /// 会话工作目录回退值 (agent 级 get_work_dir; create 时装配一次,
    /// 每实例独立 —— 原函数级 static 实现会把首实例的值固化给所有后续实例)
    std::string                  work_dir {};
};

/// 实例日志转发到宿主 agentxx.agent.log 接口表 (ctx 可空时静默丢弃;
/// noexcept —— catch 路径禁止任何可能再抛异常的操作)
inline void pluginLog(const PluginCtx* ctx, int level, const std::string& msg) {
    if (ctx && ctx->host && ctx->iface.log && ctx->iface.log->log) {
        ctx->iface.log->log(ctx->host, level, agentxx_plugin_sv(msg.data(), msg.size()));
    }
}

/// 宿主分配字符串 (跨边界内存; host 取自回调恢复的 PluginCtx)
inline char* pluginStrdup(const AgentxxHost* host, const char* s) {
    if (!host || !s) {
        return nullptr;
    }
    return host->vtable->strdup(s);
}

/// C ABI 边界异常守卫日志闭包 (捕获本实例上下文; ctx 为空 = 尚未装配,
/// 静默丢弃 —— 仅 create 最前段可能发生)
inline auto ctxGuardLogger(PluginCtx* ctx) noexcept {
    return [ctx](const char* msg) noexcept { pluginLog(ctx, 4, msg ? msg : ""); };
}

/// 宿主 toolPrompt 条目 {"depict": "...", "args": {...}} 的解析结果
struct ToolPromptText {
    std::string                                     depict;
    std::map<std::string, std::string, std::less<>> args;
};

/// 读取宿主 toolPrompt 的完整条目 (depict + 各参数说明; io 线程约束操作,
/// 宿主内部自动投递); 未配置返回空结构 (host/iface 取自本实例 PluginCtx)
inline ToolPromptText readToolPrompt(
    const AgentxxHost*                  host,
    const agentxx::plugin::AgentIfaces& iface,
    const std::string&                  toolName
) {
    ToolPromptText out;
    if (!host || !iface.config || !iface.config->get_tool_prompt) {
        return out;
    }
    char* json = iface.config->get_tool_prompt(
        host,
        agentxx_plugin_sv(toolName.data(), toolName.size())
    );
    if (!json) {
        return out;
    }
    std::string s{json};
    host->vtable->free(json);
    // 条目 JSON 结构简单固定 ({"depict": str, "args": {k: str}}),
    // 用 neograph::json 解析 (插件经 agentxx_util 传递链接, 与宿主同构)
    try {
        auto j = neograph::json::parse(s);
        out.depict = j.value("depict", std::string{});
        if (j.contains("args") && j["args"].is_object()) {
            for (const auto& [key, val] : j["args"].items()) {
                if (val.is_string()) {
                    out.args[key] = val.get<std::string>();
                }
            }
        }
    } catch (...) {
        // 解析失败按未配置处理 (回退插件默认描述)
    }
    return out;
}

/// create 时装配 agent 级工作目录到 ctx->work_dir (每实例独立; 原函数级
/// static 单例在多实例下会把首实例的值固化给所有后续实例 —— bug 已修)
inline void loadWorkDir(PluginCtx& ctx) {
    if (!ctx.host || !ctx.iface.config || !ctx.iface.config->get_work_dir) {
        return;
    }
    char* dir = ctx.iface.config->get_work_dir(ctx.host);
    if (!dir) {
        return;
    }
    ctx.work_dir = std::string{dir};
    ctx.host->vtable->free(dir);
}

/// 指定会话生效的工作目录 (execute 回调内逐次调用):
/// - 优先经 agentxx.agent.config v3 get_session_work_dir 解析 (会话绑定
///   worktree 时返回 worktree 路径, worktree 模式的路径基准切换点);
/// - 接口不可用/解析失败时回退 create 时装配的 ctx->work_dir (agent 级)
inline std::string sessionWorkDir(const PluginCtx* ctx, AgentxxPluginStringView thread_id) {
    if (ctx && ctx->host && ctx->iface.config && ctx->iface.config->get_session_work_dir
        && thread_id.data) {
        char* dir = ctx->iface.config->get_session_work_dir(ctx->host, thread_id);
        if (dir) {
            std::string s{dir};
            ctx->host->vtable->free(dir);
            if (!s.empty()) {
                return s;
            }
        }
    }
    return ctx ? ctx->work_dir : std::string{};
}

} // namespace agentxx_fs_plugin

// 插件内日志统一经 vtable 转发到宿主 (见 pluginLog), 此处重定义 XX_LOG* 宏;
// agentxx/util/log.h (经 util 头间接引入, filesystem_impl.h 使用) 也定义了
// 同名宏, 先解除再定义, 避免重定义警告 (保持本文件内插件日志宏始终生效)
#ifdef XX_LOGT
#undef XX_LOGT
#endif
#ifdef XX_LOGD
#undef XX_LOGD
#endif
#ifdef XX_LOGI
#undef XX_LOGI
#endif
#ifdef XX_LOGW
#undef XX_LOGW
#endif
#ifdef XX_LOGE
#undef XX_LOGE
#endif

#define XX_LOGT(...) ::agentxx_fs_plugin::pluginLog(0, fmt::format(__VA_ARGS__))
#define XX_LOGD(...) ::agentxx_fs_plugin::pluginLog(1, fmt::format(__VA_ARGS__))
#define XX_LOGI(...) ::agentxx_fs_plugin::pluginLog(2, fmt::format(__VA_ARGS__))
#define XX_LOGW(...) ::agentxx_fs_plugin::pluginLog(3, fmt::format(__VA_ARGS__))
#define XX_LOGE(...) ::agentxx_fs_plugin::pluginLog(4, fmt::format(__VA_ARGS__))
