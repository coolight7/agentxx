#include "chatbot_core.h"

#include <algorithm>
#include <cctype>

namespace agentxx {
namespace chatbot {

namespace {

std::string lowerCopy(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        const auto uc = static_cast<unsigned char>(c);
        out.push_back(uc >= 'A' && uc <= 'Z' ? static_cast<char>(uc + ('a' - 'A')) : c);
    }
    return out;
}

bool isSpaceChar(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}

/// 去掉尾部空白 (含换行)
std::string rtrimCopy(std::string_view text) {
    size_t end = text.size();
    while (end > 0 && isSpaceChar(text[end - 1])) {
        --end;
    }
    return std::string{text.substr(0, end)};
}

/// 围栏代码块的分隔行 (``` 或 ~~~ 开头的行)
bool isFenceDelimiter(std::string_view line) noexcept {
    size_t i = 0;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) {
        ++i;
    }
    if (line.size() - i < 3) {
        return false;
    }
    const char marker = line[i];
    if (marker != '`' && marker != '~') {
        return false;
    }
    size_t run = 0;
    while (i + run < line.size() && line[i + run] == marker) {
        ++run;
    }
    return run >= 3;
}

/// 句子切分的英文句末标点 (不含 `.`, 以免拆开数字与域名; 全角标点由
/// [splitSentences] 里的多字节分支单独识别)
bool isSentenceEnd(char c) noexcept {
    return c == '!' || c == '?' || c == ';';
}

/// 一个待打包的文本原子 (一行, 或一行里的一句)
struct Atom {
    std::string text;
    /// 本原子开始时处于哪个围栏内 (空 = 不在围栏内; 非空 = 该围栏的首行原文)
    std::string fenceBefore;
    /// 本原子结束时处于哪个围栏内
    std::string fenceAfter;
};

/// 把一行按句末标点切句 (标点保留在前一句末尾)
std::vector<std::string> splitSentences(std::string_view line) {
    std::vector<std::string> out;
    std::string              cur;
    size_t                   i = 0;
    auto                     flushCur = [&out, &cur]() {
        if (!cur.empty()) {
            out.push_back(cur);
            cur.clear();
        }
    };
    while (i < line.size()) {
        const auto uc = static_cast<unsigned char>(line[i]);
        // 全角标点 (。！？；) 的 UTF-8 编码都以 0xEF 开头
        if (uc == 0xEF && i + 2 < line.size()) {
            const auto b1 = static_cast<unsigned char>(line[i + 1]);
            const auto b2 = static_cast<unsigned char>(line[i + 2]);
            const bool isFullWidthPunct = b1 == 0xBC
                                          && (b2 == 0x81 || b2 == 0x82 || b2 == 0x9F || b2 == 0x9B);
            cur.append(line.substr(i, 3));
            i += 3;
            if (isFullWidthPunct) {
                flushCur();
            }
            continue;
        }
        cur.push_back(line[i]);
        ++i;
        if (isSentenceEnd(line[i - 1])) {
            flushCur();
        }
    }
    flushCur();
    return out;
}

/// 按码点硬切 (尽量在空白处断开, 避免把英文单词切成两半)
std::vector<std::string> hardSplit(std::string_view text, size_t limit) {
    std::vector<std::string> out;
    std::string              rest{text};
    while (utf8CodePointCount(rest) > limit) {
        auto        head = utf8Truncate(rest, limit);
        std::string chunk{head};
        // 尾部 1/4 区间里最后一个空白: 断在词边界上更易读
        const auto window = head.size() - head.size() / 4;
        auto       spacePos = std::string::npos;
        for (size_t i = head.size(); i > window; --i) {
            if (head[i - 1] == ' ') {
                spacePos = i - 1;
                break;
            }
        }
        if (spacePos != std::string::npos && spacePos > head.size() / 2) {
            chunk.assign(head.substr(0, spacePos));
        }
        if (chunk.empty()) {
            chunk.assign(head);
        }
        out.push_back(chunk);
        rest = rest.substr(chunk.size());
        // 吃掉断开处的空白, 下一段不以空白开头
        size_t skip = 0;
        while (skip < rest.size() && rest[skip] == ' ') {
            ++skip;
        }
        rest = rest.substr(skip);
    }
    if (!rest.empty()) {
        out.push_back(std::move(rest));
    }
    return out;
}

/// 文本 → 原子列表 (带围栏状态)
std::vector<Atom> toAtoms(std::string_view text, size_t sentenceSplitLimit) {
    std::vector<Atom> atoms;
    std::string       openFence; // 空 = 不在围栏内
    size_t            pos = 0;
    while (pos <= text.size()) {
        const auto nl  = text.find('\n', pos);
        const auto end = (nl == std::string_view::npos) ? text.size() : nl;
        const auto line = text.substr(pos, end - pos);
        const bool fenceLine = isFenceDelimiter(line);
        const std::string fenceBefore = openFence;
        std::string       fenceAfter  = openFence;
        if (fenceLine) {
            if (openFence.empty()) {
                fenceAfter = std::string{line};
            } else {
                fenceAfter.clear();
            }
        }
        const bool needSentenceSplit = !fenceLine && sentenceSplitLimit > 0
                                       && utf8CodePointCount(line) > sentenceSplitLimit;
        if (needSentenceSplit) {
            auto sentences = splitSentences(line);
            if (sentences.size() <= 1) {
                sentences.clear();
                sentences.emplace_back(std::string{line});
            }
            for (auto& sentence : sentences) {
                atoms.push_back(Atom{std::move(sentence), fenceBefore, fenceAfter});
            }
        } else {
            atoms.push_back(Atom{std::string{line}, fenceBefore, fenceAfter});
        }
        if (fenceLine) {
            openFence = fenceAfter;
        }
        if (nl == std::string_view::npos) {
            break;
        }
        pos = nl + 1;
    }
    return atoms;
}

/// 按给定上限打包 (不处理分段标记)
std::vector<std::string> packAtoms(const std::vector<Atom>& atoms, size_t limit) {
    std::vector<std::string> parts;
    std::vector<std::string> curLines;
    std::string              curOpenFence; // 本段开头要重开的围栏首行
    std::string              curFenceAfter;
    size_t                   curLen      = 0;
    bool                     curHasContent = false;

    auto flush = [&](bool isFinal) {
        if (!curHasContent) {
            curLines.clear();
            curLen         = 0;
            curOpenFence.clear();
            curFenceAfter.clear();
            return;
        }
        std::string part;
        for (size_t i = 0; i < curLines.size(); ++i) {
            if (i != 0) {
                part.push_back('\n');
            }
            part += curLines[i];
        }
        // 段尾补围栏关闭: 只在本段后面还有内容时补 (最后一段保持原样, 不改用户内容)
        if (!curFenceAfter.empty() && !isFinal) {
            part.push_back('\n');
            part += "```";
        }
        part = rtrimCopy(part);
        if (!part.empty()) {
            parts.push_back(std::move(part));
        }
        curLines.clear();
        curLen         = 0;
        curOpenFence.clear();
        curFenceAfter.clear();
        curHasContent = false;
    };

    auto startPart = [&](const Atom& atom) {
        curOpenFence = atom.fenceBefore;
        curFenceAfter = atom.fenceBefore;
        curLen        = 0;
        if (!curOpenFence.empty()) {
            curLines.push_back(curOpenFence);
            curLen = utf8CodePointCount(curOpenFence) + 1; // 后面还要接一行内容
        }
    };

    auto appendText = [&](const std::string& text) {
        if (!curLines.empty()) {
            curLen += 1;
        }
        curLines.push_back(text);
        curLen += utf8CodePointCount(text);
        curHasContent = true;
    };

    for (const auto& atom : atoms) {
        std::vector<Atom> pieces;
        if (limit > 0 && utf8CodePointCount(atom.text) > limit) {
            auto chunks = hardSplit(atom.text, limit);
            for (size_t i = 0; i < chunks.size(); ++i) {
                pieces.push_back(Atom{
                    std::move(chunks[i]),
                    atom.fenceBefore,
                    i + 1 == chunks.size() ? atom.fenceAfter : atom.fenceBefore,
                });
            }
        } else {
            pieces.push_back(atom);
        }

        for (const auto& piece : pieces) {
            const size_t addLen
                = utf8CodePointCount(piece.text) + (curHasContent ? 1 : 0);
            if (curHasContent && limit > 0 && curLen + addLen > limit) {
                flush(false);
            }
            if (!curHasContent && piece.text.empty()) {
                continue; // 段首空行没有意义
            }
            if (!curHasContent && curLines.empty()) {
                startPart(piece);
            }
            appendText(piece.text);
            curFenceAfter = piece.fenceAfter;
        }
    }
    flush(true);
    return parts;
}

/// 分段标记 `(i/n)`
std::string markerOf(size_t index, size_t total) {
    return "(" + std::to_string(index) + "/" + std::to_string(total) + ")";
}

} // namespace

/* ==================== UTF-8 ==================== */

size_t utf8CodePointCount(std::string_view text) noexcept {
    size_t count = 0;
    for (char c : text) {
        if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) {
            ++count;
        }
    }
    return count;
}

std::string_view utf8Truncate(std::string_view text, size_t maxCodePoints) noexcept {
    size_t count = 0;
    size_t i     = 0;
    while (i < text.size()) {
        if ((static_cast<unsigned char>(text[i]) & 0xC0) != 0x80) {
            if (count == maxCodePoints) {
                break;
            }
            ++count;
        }
        ++i;
    }
    return text.substr(0, i);
}

std::string utf8TruncateWithNotice(std::string_view text, size_t maxCodePoints) {
    if (maxCodePoints == 0 || utf8CodePointCount(text) <= maxCodePoints) {
        return std::string{text};
    }
    return std::string{utf8Truncate(text, maxCodePoints)} + "（内容过长已截断）";
}

/* ==================== 文本规整 ==================== */

std::string trimCopy(std::string_view text) {
    size_t begin = 0;
    size_t end   = text.size();
    while (begin < end && isSpaceChar(text[begin])) {
        ++begin;
    }
    while (end > begin && isSpaceChar(text[end - 1])) {
        --end;
    }
    return std::string{text.substr(begin, end - begin)};
}

std::string normalizeOutboundText(std::string_view text) {
    std::vector<std::string> lines;
    size_t                   pos = 0;
    while (pos <= text.size()) {
        const auto nl  = text.find('\n', pos);
        const auto end = (nl == std::string_view::npos) ? text.size() : nl;
        std::string line{text.substr(pos, end - pos)};
        // 行尾空白去掉 (聊天软件里没有意义), 行首缩进保留 (代码需要)
        lines.push_back(rtrimCopy(line));
        if (nl == std::string_view::npos) {
            break;
        }
        pos = nl + 1;
    }

    std::string out;
    int         blankRun = 0;
    for (auto& line : lines) {
        if (line.empty()) {
            ++blankRun;
            if (blankRun > 1) {
                continue; // 连续空行只留一个
            }
        } else {
            blankRun = 0;
        }
        if (!out.empty()) {
            out.push_back('\n');
        }
        out += line;
    }
    return trimCopy(out);
}

/* ==================== 切分 ==================== */

std::vector<SplitPart> splitText(std::string_view text, const SplitOptions& opt) {
    std::vector<SplitPart> out;
    const std::string      body = trimCopy(text);
    if (body.empty()) {
        return out;
    }
    const size_t maxParts = opt.maxParts == 0 ? 1 : opt.maxParts;
    if (opt.limit == 0) {
        out.push_back(SplitPart{body, false});
        return out;
    }
    if (utf8CodePointCount(body) <= opt.limit) {
        out.push_back(SplitPart{body, false});
        return out;
    }

    // 分段标记要占位数: 先留出余量, 保证带标记后每段仍不超过上限
    size_t effective = opt.limit;
    if (opt.marker && effective > 12) {
        effective -= 12;
    }
    const auto atoms = toAtoms(body, opt.limit);
    auto       parts = packAtoms(atoms, effective);
    if (parts.empty()) {
        out.push_back(SplitPart{body, false});
        return out;
    }

    bool truncated = false;
    if (parts.size() > maxParts) {
        parts.resize(maxParts);
        truncated = true;
    }
    if (opt.marker && parts.size() > 1) {
        const size_t total = parts.size();
        for (size_t i = 0; i < total; ++i) {
            parts[i] += "\n" + markerOf(i + 1, total);
        }
    }
    if (truncated) {
        parts.back() += "\n（后续内容过长已省略）";
    }
    for (auto& part : parts) {
        out.push_back(SplitPart{std::move(part), truncated});
    }
    return out;
}

std::vector<std::string> splitTextToParts(std::string_view text, const SplitOptions& opt) {
    std::vector<std::string> out;
    for (auto& part : splitText(text, opt)) {
        out.push_back(std::move(part.text));
    }
    return out;
}

/* ==================== 命令 ==================== */

const char* commandModeName(CommandMode mode) noexcept {
    switch (mode) {
        case CommandMode::Local:
            return "local";
        case CommandMode::Model:
            return "model";
        case CommandMode::Action:
            return "action";
    }
    return "local";
}

std::vector<CommandDef> builtinCommands() {
    std::vector<CommandDef> defs;

    // 帮助: 命令清单由引擎按这张表拼成 {commandList}
    {
        CommandDef def;
        def.name  = "/axx-help";
        def.mode  = CommandMode::Local;
        def.reply = "可用命令: {commandList}\n"
                    "直接发命令名即可, 不进模型; 命令后面可以跟一段文字, 作为 {args} 交给模型命令。";
        defs.push_back(std::move(def));
    }
    // 会话信息: 排查「为什么不回复」先看这里
    {
        CommandDef def;
        def.name  = "/axx-info";
        def.mode  = CommandMode::Local;
        def.reply = "人设 {persona} · 模式 {mode} · 通道 {channelId}\n"
                    "会话 {chat} ({chatType}) · 账号 {account} · 已运行 {uptime}\n"
                    "收到 {received} 条 · 回复 {sent} 条 · 丢弃 {dropped} 条 · 重复 {duplicated} 条";
        defs.push_back(std::move(def));
    }
    // 让模型说一句: 不打字也能确认「模型 → 出站」这一段是通的
    {
        CommandDef def;
        def.name   = "/axx-hello";
        def.mode   = CommandMode::Model;
        def.prompt = "请向 {sender} 打个招呼, 一句话即可。{args}";
        defs.push_back(std::move(def));
    }
    // 打断当前一轮 (没有回复: 没有正在跑的轮次时也没什么好说的)
    {
        CommandDef def;
        def.name   = "/axx-stop";
        def.mode   = CommandMode::Action;
        def.action = "cancel";
        defs.push_back(std::move(def));
    }
    return defs;
}

ParsedCommand parseCommand(std::string_view text, const std::vector<CommandDef>& defs) {
    ParsedCommand result;
    size_t         begin = 0;
    while (begin < text.size() && isSpaceChar(text[begin])) {
        ++begin;
    }
    const auto body = text.substr(begin);
    if (body.empty() || body[0] != '/') {
        return result;
    }
    const auto bodyLower = lowerCopy(body);
    for (const auto& def : defs) {
        if (def.name.empty() || def.name[0] != '/') {
            continue;
        }
        const auto nameLower = lowerCopy(def.name);
        if (bodyLower.size() < nameLower.size()
            || bodyLower.compare(0, nameLower.size(), nameLower) != 0) {
            continue;
        }
        // 命令名后必须是空白或行尾, 否则 `/axx-information` 会被 `/axx-info` 命中
        const bool atEnd = bodyLower.size() == nameLower.size();
        if (!atEnd && !isSpaceChar(bodyLower[nameLower.size()])) {
            continue;
        }
        if (!result.matched || def.name.size() > result.name.size()) {
            result.matched = true;
            result.name    = def.name;
            result.args    = atEnd ? std::string{}
                                   : trimCopy(body.substr(nameLower.size()));
        }
    }
    return result;
}

/* ==================== 模板 ==================== */

std::string renderTemplate(
    std::string_view                                       tmpl,
    const std::map<std::string, std::string, std::less<>>& vars
) {
    std::string out;
    out.reserve(tmpl.size());
    size_t i = 0;
    while (i < tmpl.size()) {
        const char c = tmpl[i];
        if (c == '{') {
            if (i + 1 < tmpl.size() && tmpl[i + 1] == '{') {
                out.push_back('{');
                i += 2;
                continue;
            }
            const auto close = tmpl.find('}', i + 1);
            if (close != std::string_view::npos && close > i + 1) {
                const auto inner = tmpl.substr(i + 1, close - i - 1);
                if (inner.find('{') == std::string_view::npos) {
                    auto it = vars.find(std::string{inner});
                    if (it != vars.end()) {
                        out += it->second;
                        i = close + 1;
                        continue;
                    }
                }
            }
            out.push_back(c);
            ++i;
            continue;
        }
        if (c == '}' && i + 1 < tmpl.size() && tmpl[i + 1] == '}') {
            out.push_back('}');
            i += 2;
            continue;
        }
        out.push_back(c);
        ++i;
    }
    return out;
}

/* ==================== 输入块 ==================== */

std::string renderInputBlock(
    const InputBlockOptions&             opt,
    const std::vector<InputMessageView>& messages
) {
    std::string out;
    if (opt.includeContextHint) {
        const std::string name = displayNameOf(opt.chatName, opt.chatId);
        if (opt.type == ChatType::Group) {
            out += opt.platformLabel + "群「" + name + "」(群 id 尾号 " + chatIdTail(opt.chatId)
                   + ")";
        } else {
            out += opt.platformLabel + "上私聊「" + name + "」";
        }
        out += "; 下面是最近收到的消息：";
        if (!opt.hintExtra.empty()) {
            out += "\n" + opt.hintExtra;
        }
        out += "\n";
    }
    for (const auto& msg : messages) {
        const std::string sender = displayNameOf(msg.senderName, std::string{}, 64);
        std::string       line   = "[#" + std::to_string(msg.index) + "] " + sender;
        if (!msg.senderRole.empty()) {
            line += "(" + msg.senderRole + ")";
        }
        if (msg.mentionedMe) {
            line += " @机器人";
        }
        if (msg.hasQuote) {
            line += " (引用了 " + displayNameOf(msg.quotedSender, std::string{}, 64) + " 的消息)";
        }
        if (!msg.text.empty()) {
            line += ": ";
            line += msg.text;
        }
        out += line;
        out.push_back('\n');
        for (const auto& att : msg.attachments) {
            std::string attLine = "[#" + std::to_string(msg.index) + ":附件] " + att.fileName
                                  + " (";
            attLine += att.local ? "本地: " : "地址: ";
            attLine += att.location;
            if (!att.note.empty()) {
                attLine += "; ";
                attLine += att.note;
            }
            attLine += ")";
            out += attLine;
            out.push_back('\n');
            if (!att.inlineText.empty()) {
                out += "[#" + std::to_string(msg.index) + ":附件内容开始] " + att.fileName + "\n";
                out += att.inlineText;
                if (att.inlineText.back() != '\n') {
                    out.push_back('\n');
                }
                out += "[#" + std::to_string(msg.index) + ":附件内容结束]\n";
            }
        }
    }
    return rtrimCopy(out);
}

/* ==================== 引用编号 ==================== */

int parseReplyTo(std::string_view text) noexcept {
    size_t begin = 0;
    size_t end   = text.size();
    while (begin < end && isSpaceChar(text[begin])) {
        ++begin;
    }
    while (end > begin && isSpaceChar(text[end - 1])) {
        --end;
    }
    if (begin < end && text[begin] == '#') {
        ++begin;
    }
    if (begin >= end) {
        return 0;
    }
    int    value = 0;
    size_t digits = 0;
    for (size_t i = begin; i < end; ++i) {
        const char c = text[i];
        if (c < '0' || c > '9') {
            return 0;
        }
        value = value * 10 + (c - '0');
        ++digits;
        if (digits > 6) {
            return 0;
        }
    }
    if (value <= 0 || value > 9999) {
        return 0;
    }
    return value;
}

std::string chatIdTail(std::string_view chatId, size_t tailLen) {
    if (chatId.size() <= tailLen || tailLen == 0) {
        return std::string{chatId};
    }
    // 切分位置要落在 UTF-8 码点边界上
    size_t begin = chatId.size() - tailLen;
    while (begin < chatId.size()
           && (static_cast<unsigned char>(chatId[begin]) & 0xC0) == 0x80) {
        ++begin;
    }
    return std::string{chatId.substr(begin)};
}

std::string displayNameOf(std::string_view name, std::string_view id, size_t headLen) {
    if (!name.empty()) {
        return std::string{name};
    }
    if (id.empty()) {
        return std::string{};
    }
    return std::string{utf8Truncate(id, headLen)};
}

} // namespace chatbot
} // namespace agentxx
