#pragma once

/// chatbot 配置: 三段 (accounts / personas / behavior) → 强类型结构 + 校验
///
/// 写法与 `model.list` / `model.use` 同构, 详见 `resource/history/chatbot/plan.md` §5。
/// 关键词命令不在配置里: 由插件内置, 见 [builtinCommands]。
///
/// 两个入口:
/// - [parseConfig]: 从 `plugin.list[].args` 的 JSON 解析 (主程序原样转交);
/// - [mergeConfigJson]: 把可选的 `plugin.list[].config` 文件内容合并进来
///   (args 覆盖文件: 映射逐键、列表整体覆盖, 与主程序分层配置同一规则)。
///
/// 校验在 `start` 里、建根之前做完: 任何一条不过就拒绝启动 (不留半启动状态)。
#include "chatbot_core.h"
#include "chatbot_types.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace agentxx {
namespace chatbot {

/// 一个平台账号 (= 一份凭证 + 一条长连接)
struct AccountConfig {
    /// 配置里的标识, 人设按它对接 (`[A-Za-z0-9_-]`)
    std::string id;
    /// 平台类型 (当前只有 `qq`)
    std::string type = "qq";
    std::string appId;
    std::string appSecret;
    /// 沙箱环境 (平台侧权限不同, 接口域名相同)
    bool sandbox = false;
    /// 覆盖接口地址 (灰度/抓包调试用; 空 = 平台默认)
    std::string apiBase;
    /// 网关分片 (QQ 的 Identify 报文里带, 默认 [0,1])
    std::vector<int32_t> shard{0, 1};
    /// 主动消息频控覆盖 (0 = 用平台默认值, 插件再按 70% 保守设)
    int32_t groupPerMinute  = 0;
    int32_t singlePerMinute = 0;
};

/// 一条「人设对接某会话」的规则
struct BindRule {
    std::string accountId;
    ChatType    type = ChatType::Group;
    std::string chatId;
    /// `chatId` 为 `*` = 该账号该类型下的所有会话
    bool     wildcardId = false;
    ChatMode mode       = ChatMode::Mention;
};

/// 人设的提示词段
struct PersonaPrompt {
    std::string systemPrompt;
    /// 追加段 (key → 内容; 框架的 `prompt.append` 支持按 key 追加)
    std::vector<std::pair<std::string, std::string>> append;
};

/// 人设的插件挂载策略 (与 yaml `plugin.mount` 同构)
struct PersonaMount {
    /// `none` / `all` / `listed`
    std::string mode = "none";
    std::vector<std::string> list;
    /// 各插件的 args 覆盖 (键 = 插件名)
    std::string argsJson;
};

/// 一个人设 (= 一个 root agent)
struct PersonaConfig {
    std::string name;
    /// 可选模型 (必须存在于主程序的可用模型表; 空 = 用主程序当前模型)
    std::string model;
    PersonaPrompt prompt;
    /// 工具白名单 (工具名, 不是插件名); 空 = 不过滤
    std::vector<std::string> tools;
    PersonaMount mount;
    std::vector<BindRule> binds;

    /// 该人设的会话是否落盘 (LRU 回收通道后靠会话库恢复上下文, 默认开)
    bool enableSessionStore = true;
    /// 是否启用上下文压缩 (长群聊会触发, 默认继承主程序)
    std::optional<bool> enableSummarization;
};

/// 通用行为 (与具体人设无关)
struct BehaviorConfig {
    /// 多条消息合并窗口 (毫秒; 仅 `always` 模式用)
    int32_t aggregateWindowMs = 800;
    /// 一轮内多条回复之间的间隔
    int32_t sendIntervalMs = 350;
    /// 单条发送文本上限 (UTF-8 码点; 0 = 不切分)
    int32_t messageSplitLimit = 1500;
    /// 切出多条时追加 `(i/n)` 分段标记
    bool splitMarker = true;
    /// 单条最多切成几段
    int32_t maxSplitParts = 8;
    /// 群消息是否用 markdown 形态发 (单聊恒为纯文本)
    bool markdown = false;
    /// 一轮最多发几条
    int32_t maxRepliesPerTurn = 5;
    /// 每人设活跃会话上限 (= 每根 64 通道)
    int32_t maxChatsPerPersona = 64;
    /// 每会话保留最近多少条原始消息 (供 `chatbot_history`)
    int32_t historyKeep = 40;
    /// 附件接收策略: `auto` / `url` / `download`
    std::string attachmentsFetch = "auto";
    /// 单次投给会话的附件上限 (主程序上限就是 5)
    int32_t attachmentsMaxPerMessage = 5;
    /// 普通文件多大以内把内容直接内联进输入 (字节; 0 = 不内联)
    ///
    /// - 文本文件 (UTF-8, 不含 NUL) 超限或不是文本时只在输入里给路径,
    ///   由模型用文件系统工具自己读 —— 而人设根没挂文件系统插件时就读不到,
    ///   所以小文件默认内联, 让"看看这个文件是什么"这种问法开箱即用
    int32_t attachmentsInlineTextMaxBytes = 64 * 1024;
    /// 是否把 `chatbot_*` 工具也注册到内置 root (调试用)
    bool exposeToolsOnHost = false;
    /// 被动回复次数用尽后: `active`(转主动) / `truncate`(放弃) / `fail`(报错给模型)
    std::string overflowPolicy = "active";
    /// 模型没调工具时是否兜底发正文: `mention` / `always` / `never`
    std::string fallbackReply = "mention";
    /// 每轮输入头部是否带「你在哪个群」的元信息段
    bool injectContextHint = true;
    /// 有旧内容没发出时, 下次有人说话顺带补 1 条
    bool flushStaleOnNextMessage = true;
    /// 工具要人批准时怎么办: `auto-allow` / `auto-deny`
    std::string humanInLoop = "auto-allow";
    /// 每会话聚合缓冲上限
    int32_t aggregateMaxItems = 30;
    int32_t aggregateMaxChars = 12000;
    /// 出站队列长度上限 (超出丢最旧)
    int32_t outboundQueueMax = 200;
    /// 每账号去重环容量
    int32_t dedupeCapacity = 512;
    /// 单条入站原文超过多少码点就截断 (原文仍留在历史里)
    int32_t inboundMaxChars = 20000;
    /// 是否注册调试工具 (`chatbot_debug_inject` / `chatbot_debug_outbox`)
    ///
    /// - 只给测试与本地演示用: 直接向调度层投一条假消息、回看平台收到了什么;
    /// - 默认关 (生产环境不暴露这两个工具给模型)
    bool debugTools = false;
};

/// 完整配置
struct ChatbotConfig {
    std::vector<AccountConfig> accounts;
    std::vector<PersonaConfig> personas;
    BehaviorConfig             behavior;
    /// 命令表 (插件内置, 由 [builtinCommands] 填; 不从配置读)
    std::vector<CommandDef>    commands;

    /// 按 id 找账号 (找不到返回空指针)
    const AccountConfig* findAccount(std::string_view id) const;
    /// 按名字找人设
    const PersonaConfig* findPersona(std::string_view name) const;
};

/// 解析结果
struct ConfigError {
    /// 出错的配置路径 (如 `personas[0].bind[1].account`)
    std::string where;
    /// 给人看的原因
    std::string message;

    bool empty() const noexcept {
        return where.empty() && message.empty();
    }
};

/// 从 `plugin.list[].args` 的 JSON 文本解析配置
///
/// - 缺段按默认值处理 (`accounts` / `personas` 为空会在 [validateConfig] 里被拒)
/// - `appId` / `appSecret` 支持 `appIdFile` / `appSecretFile` 从 `.env` 形态的文件读
bool parseConfig(std::string_view argsJson, ChatbotConfig& out, ConfigError& err);

/// 把外部配置文件 (yaml, 四段同上) 合并进 args 的 JSON 对象
///
/// - 文件不存在或解析失败时返回 false 并写 `err`; 调用方决定是否致命
/// - 合并规则: 映射逐键递归合并 (args 覆盖文件), 列表整体覆盖
bool mergeConfigFile(
    std::string_view        filePath,
    std::string&            argsJsonInOut,
    ConfigError&            err
);

/// 完整校验 (顺序与设计文档 §5.5 一致; 通过后配置才可用于建根)
///
/// - `maxRoots`: 主程序的根数上限 (含内置根); 人设数 + 1 不得超过它
bool validateConfig(ChatbotConfig& config, int32_t maxRoots, ConfigError& err);

/// 解析 `.env` 形态的文本 (`KEY=VALUE` 每行一条), 取某个键的值
///
/// - 忽略空行与 `#` 注释, 允许 `export KEY=VALUE`, 值两端引号会被去掉
std::string readKeyFromEnvText(std::string_view text, std::string_view key);

/// 从文件读 `.env` 形态的键值 (读不到返回空)
std::string readKeyFromEnvFile(std::string_view path, std::string_view key);

/* ==================== 路由表 ==================== */

/// 路由结果
struct RouteResult {
    bool        found = false;
    /// 归属人设名
    std::string personaName;
    ChatMode    mode = ChatMode::Mention;
    /// 命中通配规则 (`chats: [{type: any, id: "*"}]`)
    bool wildcard = false;
};

/// `ChatKey` → 人设 + 触发模式
///
/// - 精确项优先, 通配项兜底; 表在 `start` 时一次性建好, 运行期只读
class RouterTable {
public:

    void build(const ChatbotConfig& config);

    RouteResult route(const ChatKey& key) const;

    /// `chatbot_set_mode` 的运行期覆盖 (不写回 yaml)
    void setModeOverride(std::string_view chatKey, ChatMode mode);

    /// 取生效模式 (覆盖优先, 其次配置; 找不到返回 `Mention`)
    ChatMode effectiveMode(const ChatKey& key, const RouteResult& base) const;

    /// 列出某个人设对接的会话键 (已配置的精确项, 供诊断/状态查询)
    std::vector<std::string> chatKeysOf(std::string_view personaName) const;

    /// 全部配置里的会话键
    const std::vector<std::string>& allChatKeys() const noexcept {
        return preciseKeys_;
    }

    size_t ruleCount() const noexcept {
        return precise_.size() + wildcard_.size();
    }

private:

    struct PreciseEntry {
        RouteResult result;
    };

    /// 键 = `account|type|chatId`
    std::map<std::string, PreciseEntry, std::less<>>      precise_;
    /// 键 = `account|type` (通配项)
    std::map<std::string, RouteResult, std::less<>>       wildcard_;
    std::map<std::string, ChatMode, std::less<>>          overrides_;
    std::vector<std::string>                              preciseKeys_;
};

} // namespace chatbot
} // namespace agentxx
