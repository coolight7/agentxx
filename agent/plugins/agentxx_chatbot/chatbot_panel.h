#pragma once

/// 侧边栏「机器人」栏的数据 (agent 侧整理 → 跨端事件 → client 侧渲染)
///
/// 分工:
/// - **agent 侧** (chatbot_engine 的 `panelSnapshot()` + 入口的发布逻辑) 把运行态
///   整理成 [PanelSnapshot], 经事件总线 topic `agentxx_chatbot.panel` 发布;
///   主程序把该主题转发成 `WirePluginData{plugin,event,data}`, 客户端插件在
///   `AGENTXX_CLIENT_EVT_PLUGIN_DATA` 里收到同一份 JSON;
/// - **client 侧** (chatbot_client.cpp) 收下快照, 用 chatbot_panel_ui 画成面板。
///
/// 本头只依赖标准库与 `utilxx_base::Json`, 不引用主程序头 (插件边界), 也不碰网络,
/// 因此可以直接编进测试 (`chatbot_core` 模块) 做纯逻辑回归。
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace agentxx {
namespace chatbot {

/// 一个已经创建出来的机器人会话 (= 某个群/私聊在一个账号下的一条通道)
///
/// 字段用途:
/// - `title` / `persona` / `chatType` 给面板第二行做副标题;
/// - `running` 决定左侧是否显示加载动画 (正在跑一轮);
/// - `queued` 就是面板右侧那个数字 (还在聚合缓冲里等着的消息条数);
/// - `sessionId` / `channelId` 只在排错时看 (面板不显示)。
struct PanelSessionView {
    std::string chatKey;    ///< `account|group|chatId`
    std::string title;      ///< 群名 / 昵称 (空时 agent 侧已退回 `群聊-尾号`)
    std::string persona;    ///< 归属人设名
    std::string account;    ///< 平台账号 id
    std::string chatType;   ///< `group` / `private`
    std::string mode;       ///< `mention` / `always` / `off`
    std::string sessionId;  ///< 会话 id (`chatbot_<人设>_<账号>_…`)
    int64_t     channelId    = 0;
    bool        ready        = false; ///< 通道是否已就绪 (收到 READY)
    bool        running      = false; ///< 是否正在执行一轮
    size_t      queued       = 0;     ///< 排队消息条数
    uint64_t    turns        = 0;     ///< 已跑几轮
    uint64_t    sent         = 0;     ///< 已发几条回复
    int64_t     lastTurnMs   = 0;     ///< 上一轮耗时 (毫秒)
    int64_t     lastActiveMs = 0;     ///< 最近一次活动 (本机单调时钟毫秒, 仅用于排序)
};

/// 一个人设 (= 一个 root agent)
struct PanelPersonaView {
    std::string name;
    std::string model;      ///< 空 = 用主程序当前模型
    std::string promptHead; ///< 提示词第一行 (面板显示一行摘录, 已截断)
    std::string rootId;     ///< 人设根 id (空 = 没挂上)
    bool        mounted    = false;
    size_t      chats      = 0; ///< 当前活跃会话数
    size_t      chatBudget = 0; ///< 人设的活跃会话上限
    size_t      binds      = 0; ///< 配置里对接的会话规则数
    size_t      tools      = 0; ///< 工具白名单条数 (0 = 不过滤)
};

/// 一个平台账号的连接状态
struct PanelAccountView {
    std::string id;
    std::string type;   ///< `qq` / `fake`
    std::string state;  ///< `connecting` / `online` / `reconnecting` / `disabled`
    std::string reason; ///< 平台给的原因 (可空)
};

/// 一份面板快照 (一次事件推送的完整内容)
struct PanelSnapshot {
    /// 刷新计数 (1 起): 客户端用它取加载动画的当前帧, 所以同一份数据也要换帧号
    uint32_t tick        = 0;
    std::string startupTime;
    std::vector<PanelAccountView> accounts;
    std::vector<PanelPersonaView> personas;
    std::vector<PanelSessionView> sessions;
    uint64_t received = 0; ///< 全局收到的消息数
    uint64_t sent     = 0; ///< 全局发出的回复数
    uint64_t failed   = 0; ///< 投递失败次数
};

/// 快照 JSON 结构版本 (客户端按它判断能不能认; 不认识时只显示不可用提示)
constexpr int kPanelSnapshotVersion = 1;

/* ==================== 序列化 ==================== */

/// 快照 → JSON 文本 (agent 侧发布用)
std::string panelSnapshotJson(const PanelSnapshot& snapshot);

/// JSON 文本 → 快照 (client 侧解析用)
///
/// - 结构版本比 [kPanelSnapshotVersion] 大、或 JSON 非法时返回 false 且不改 `out`;
/// - 未知字段忽略 (以后 agent 侧追加字段时老客户端仍能显示)
bool parsePanelSnapshot(std::string_view json, PanelSnapshot& out);

/* ==================== 展示文案 (面板与测试共用) ==================== */

/// 会话一行的副标题: `assistant · 群聊 · always`
std::string panelSessionSubtitle(const PanelSessionView& session);

/// 人设一行的副标题: `会话 2/64 · 绑定 3`, 没挂上时追加 `· 未挂载`
std::string panelPersonaSubtitle(const PanelPersonaView& persona);

/// 人设一行的右侧文字: 模型名 (空则 `默认模型`)
std::string panelPersonaModelText(const PanelPersonaView& persona);

/// 人设的副信息一行: 模型 + `会话 2/64 · 绑定 3` (`gpt-x · 会话 2/64 · 绑定 3`)
std::string panelPersonaDetail(const PanelPersonaView& persona);

/// 会话右侧的排队条数文本 (`0` 也表示"没有积压")
std::string panelSessionQueueText(const PanelSessionView& session);

/// 账号列表项文本 (不带 `|- ` 前缀): `qq_main online`, 断开时带原因
std::string panelAccountItemText(const PanelAccountView& account);

/// 按码点截断 (面板是窄栏, 名字太长会挤掉右侧的排队数)
std::string panelTruncate(std::string_view text, size_t maxCodePoints);

} // namespace chatbot
} // namespace agentxx
