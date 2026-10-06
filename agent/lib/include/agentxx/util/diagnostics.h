#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace agentxx {
namespace agent {
class AgentContext;
}
namespace util {

/// 诊断包 (计划 OBS-4 / STO-13 的导出部分)
///
/// 目的: 用户报障时给出一份**自包含**的文本 (可直接贴出来), 包含环境、配置、
/// 装配、会话摘要、关键指标与日志尾部 —— 而不需要对方复述界面或翻日志文件。
///
/// 内容边界 (安全约定):
/// - 配置只导出装配快照口径的字段 (API Key 只输出"是否已设置", 参数只输出键名);
/// - 会话默认只导出**计数与用量** (消息条数/角色分布/上下文 token/账本聚合),
///   不含消息正文; 需要正文时显式打开 [DiagnosticsOptions::includeMessages];
/// - 日志尾部经 [redactSecrets] 处理 (屏蔽常见凭据形态), 并且**不导出环境变量**。
struct DiagnosticsOptions {
    /// 是否包含会话消息正文 (默认关闭; 打开时最多 [maxMessageChars] 字符)
    bool   includeMessages = false;
    /// 单条消息正文上限 (字符)
    size_t maxMessageChars = 200;
    /// 日志尾部行数 (需先启用 [enableLogCapture]; 0 = 不含日志)
    size_t logTailLines = 100;
    /// 工具清单/插件清单是否逐项列出 (关闭时只给计数)
    bool detailedLists = true;
};

/// 生成诊断包文本 (Markdown 风格; 各段失败只省略该段, 不抛异常)
///
/// - `sessionId` 为空时跳过会话段;
/// - 依赖 `AgentContext` 中已装配的部分 (配置/插件/会话管理器/存储), 缺哪部分跳过哪段。
std::string buildDiagnosticsText(
    agent::AgentContext&                  ctx,
    std::string_view                      sessionId    = {},
    const DiagnosticsOptions&             options      = {}
);

/// 安装日志环形缓冲 (进程级; 重复调用只更新容量)
/// - 诊断包要能带上"最近的日志", 但日志文件路径/滚动策略属于客户端配置,
///   因此这里提供进程内的最近日志捕获 (默认不安装, 不占内存)
void enableLogCapture(size_t capacity = 500);
/// 关闭日志捕获并释放缓冲
void disableLogCapture();
/// 最近的日志行 (按时间顺序; limit == 0 返回全部已捕获行)
std::vector<std::string> recentLogLines(size_t limit = 0);
/// 清空已捕获日志
void clearCapturedLogs();

/// 屏蔽文本中的常见凭据形态 (计划 OBS-2/诊断包安全边界)
///
/// - `key=value` / `key: value` 形式的 api_key / token / secret / password /
///   authorization 值整体替换为 `***`;
/// - `Authorization: Bearer xxx` 之类的凭证段替换;
/// - `sk-` / `ghp_` / `github_pat_` 前缀的长 token 替换。
/// - 只做形态匹配, 不保证穷尽; 用途是"日志尾部不直接带出凭据", 不是安全存储。
std::string redactSecrets(std::string_view text);

} // namespace util
} // namespace agentxx
