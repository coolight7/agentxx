#pragma once

/// chatbot 纯逻辑: 切分 / 命令解析 / 输入块渲染 / 去重环 / 引用编号
///
/// 这一层**不碰网络、不碰主程序**, 全部是可直接单测的纯函数 (测试模块 `chatbot_core`
/// 直接 include 本头)。调度层负责把它们串起来。
///
/// 设计见 `resource/history/chatbot/plan.md` §10.2 / §9 / §7.4 / §7.5。
#include "chatbot_types.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace agentxx {
namespace chatbot {

/* ==================== UTF-8 基础 ==================== */

/// UTF-8 文本的码点个数 (非法字节按单字节计入, 不抛异常)
size_t utf8CodePointCount(std::string_view text) noexcept;

/// 按码点边界截断 (超长时返回前 `maxCodePoints` 个码点, 不会切断多字节序列)
std::string_view utf8Truncate(std::string_view text, size_t maxCodePoints) noexcept;

/// 截断并给出说明后缀 (未截断时原样返回)
///
/// - `args`:
///     - [text] 原文
///     - [maxCodePoints] 上限码点数
///
/// - `return` 未超限 = 原文拷贝; 超限 = 前 N 个码点 + "（内容过长已截断）"
std::string utf8TruncateWithNotice(std::string_view text, size_t maxCodePoints);

/* ==================== 文本规整 ==================== */

/// 去掉首尾空白 (空格 / 制表 / 换行)
std::string trimCopy(std::string_view text);

/// 出站文本规整: 去首尾空白 + 把 3 行以上连续空行折叠成 1 个空行
std::string normalizeOutboundText(std::string_view text);

/* ==================== 长文本切分 (plan.md §10.2) ==================== */

/// 切分参数
struct SplitOptions {
    /// 单条最多多少字符 (按 UTF-8 码点计; 0 = 不切分, 原样返回一条)
    size_t limit = 1500;
    /// 最多切成几段 (超出部分丢弃并追加省略说明)
    size_t maxParts = 8;
    /// 切出多条时在末尾追加 `(1/3)` 形式的分段标记
    bool marker = true;
    /// ``` 围栏内的内容尽量整块保留; 必须切开时在段尾补围栏关闭、段首补围栏重开
    bool protectFences = true;
};

/// 一条切分结果
struct SplitPart {
    std::string text;
    /// 本条是否因超出 `maxParts` 而被截断 (最后一段为 true 时末尾已带省略说明)
    bool truncated = false;
};

/// 把长文本按「段落 → 句子 → 硬切」三级策略切成多条
///
/// - 优先按空行分段累加; 单段超限再按中英文句末标点切; 仍超限按码点硬切
/// - 围栏代码块不按句子切 (避免把代码拆得不可读), 必须切开时补围栏
/// - 切出多条时每段末尾追加 `(i/n)` (可关)
///
/// - `return` 至少一条 (输入为空时返回空列表)
std::vector<SplitPart> splitText(std::string_view text, const SplitOptions& opt);

/// 同 [splitText], 只要文本 (便于调用方直接拼接/计数)
std::vector<std::string> splitTextToParts(std::string_view text, const SplitOptions& opt);

/* ==================== 关键词命令 (plan.md §9) ==================== */

/// 命令的执行方式
enum class CommandMode : uint8_t {
    /// 插件直接回复模板, 不进模型
    Local = 0,
    /// 构造一条专用输入投给 agent
    Model = 1,
    /// 执行插件动作 (cancel / clear)
    Action = 2,
};

const char* commandModeName(CommandMode mode) noexcept;
bool        commandModeFromString(std::string_view text, CommandMode& out) noexcept;

/// 一条命令定义 (来自配置)
struct CommandDef {
    /// 命令名 (`/axx-info`), 必须以 `/` 开头
    std::string name;
    CommandMode mode = CommandMode::Local;
    /// `Local` 的回复模板
    std::string reply;
    /// `Model` 的提示词模板
    std::string prompt;
    /// `Action` 的动作名 (`cancel` / `clear`)
    std::string action;
};

/// 命令匹配结果
struct ParsedCommand {
    bool        matched = false;
    /// 命中的命令名 (原样大小写)
    std::string name;
    /// 命令名之后的剩余文本 (已去首尾空白)
    std::string args;
};

/// 在一条消息里匹配命令
///
/// - 命令名必须以 `/` 开头, 允许前置空白
/// - 大小写不敏感, 命令名后必须是空白或行尾 (`/axx-info` 命中, `/axx-information` 不命中)
/// - 同一条消息只匹配一个命令, 取**最长**命中
ParsedCommand parseCommand(std::string_view text, const std::vector<CommandDef>& defs);

/* ==================== 模板渲染 ==================== */

/// 简单占位符渲染: `{name}` → 值
///
/// - 未知变量原样保留 (便于用户写 `{whatever}` 不报错)
/// - `{{` / `}}` 渲染成单个花括号 (要写字面量时用)
std::string renderTemplate(
    std::string_view                                tmpl,
    const std::map<std::string, std::string, std::less<>>& vars
);

/* ==================== 输入块渲染 (plan.md §7.4) ==================== */

/// 一条附件在输入文本里的展示行
struct AttachmentLine {
    std::string fileName;
    /// 本地绝对路径或 URL
    std::string location;
    /// true = 本地路径, false = 远端地址
    bool local = true;
    /// 附加说明 ("未随消息传递" / "下载失败(原因)")
    std::string note;
};

/// 一条消息在输入文本里的展示形态
struct InputMessageView {
    /// 编号 (1 起; 也就是 `chatbot_send_message` 的 `replyTo`)
    int         index = 1;
    std::string senderName;
    /// `owner` / `admin` / `member` / 空
    std::string senderRole;
    bool        mentionedMe = false;
    bool        hasQuote    = false;
    std::string quotedSender;
    std::string text;
    std::vector<AttachmentLine> attachments;
};

/// 输入块标题参数
struct InputBlockOptions {
    ChatType    type = ChatType::Group;
    /// 平台类型 (`QQ` 之类, 只用于标题文案)
    std::string platformLabel = "QQ";
    /// 群名 / 用户昵称
    std::string chatName;
    /// 群 id 或用户 id (为展示时取尾号/前 8 位)
    std::string chatId;
    /// 是否在开头加"你在哪个群"的会话元信息段
    bool includeContextHint = true;
    /// 追加在元信息段之后的一句话 (如"上一条消息投递失败")
    std::string hintExtra;
};

/// 渲染一次会话输入 (模型看到的那段文本)
std::string renderInputBlock(
    const InputBlockOptions&          opt,
    const std::vector<InputMessageView>& messages
);

/* ==================== 引用编号 ==================== */

/// 解析工具参数里的 `replyTo`
///
/// - 接受 `"3"` / `"#3"` / `" 3 "`; 空串或非法返回 0 (0 = 不引用)
int parseReplyTo(std::string_view text) noexcept;

/// 展示用的会话/账号尾号 (`B2C3D4E5F6…` → `C3D4`)
std::string chatIdTail(std::string_view chatId, size_t tailLen = 4);

/// 展示用的短名 (名称空时退回 id 前 8 位)
std::string displayNameOf(std::string_view name, std::string_view id, size_t headLen = 8);

/* ==================== 去重环 (plan.md §7.5) ==================== */

/// 固定容量的「最近见过的键」集合
///
/// 平台可能重复推送同一条消息, 这里保留最近 N 个键: 命中的丢弃并计数, 未命中的记入。
class DedupeRing {
public:

    explicit DedupeRing(size_t capacity = 512) :
        capacity_(capacity == 0 ? 1 : capacity) {}

    /// 记入一个键
    ///
    /// - `return` true = 之前已经见过 (调用方应丢弃本条)
    bool seenOrAdd(std::string_view key) {
        if (key.empty()) {
            return false;
        }
        if (keys_.contains(std::string{key})) {
            ++duplicateCount_;
            return true;
        }
        order_.emplace_back(std::string{key});
        keys_.insert(order_.back());
        while (order_.size() > capacity_) {
            keys_.erase(order_.front());
            order_.pop_front();
        }
        return false;
    }

    bool     contains(std::string_view key) const {
        return keys_.contains(std::string{key});
    }

    size_t size() const noexcept {
        return order_.size();
    }

    size_t capacity() const noexcept {
        return capacity_;
    }

    uint64_t duplicateCount() const noexcept {
        return duplicateCount_;
    }

    void clear() {
        order_.clear();
        keys_.clear();
        duplicateCount_ = 0;
    }

private:

    size_t                                   capacity_ = 512;
    std::deque<std::string>                  order_;
    std::unordered_set<std::string>           keys_;
    uint64_t                                 duplicateCount_ = 0;
};

} // namespace chatbot
} // namespace agentxx
