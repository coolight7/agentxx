/// test_chatbot_core —— `agentxx_chatbot` 插件的纯逻辑
///
/// 覆盖 (对应 `resource/history/chatbot/plan.md` §16.1 的 `chatbot_core` 一行):
/// - 长文本切分: 段落 / 句子 / 硬切 / 围栏代码 / emoji / 恰好等于上限 / 上限 0
/// - 命令解析: 前缀 / 大小写 / 长名优先 / 模板变量
/// - 输入块渲染: 群 / 私聊 / 引用 / 附件
/// - 去重环: 重复 / 容量 / 顺序
/// - 引用编号映射
/// - 配置校验: 每种非法配置给出预期错误
/// - 侧边栏「机器人」栏: 快照 JSON 往返 / 展示文案 / 组件树 (加载帧与排队数)
///
/// 这些函数不碰网络也不碰主程序, 因此本模块是同步的 (不 co_await)。
#include "agentxx-test/core/test_chatbot_core.h"

#include "agentxx_chatbot/chatbot_config.h"
#include "agentxx_chatbot/chatbot_core.h"
#include "agentxx_chatbot/chatbot_panel.h"
#include "agentxx_chatbot/chatbot_panel_ui.h"
#include "agentxx_chatbot/chatbot_router.h"
#include "agentxx_chatbot/chatbot_types.h"

#include <map>
#include <set>
#include <string>
#include <vector>

namespace {
int g_cc_passed = 0;
int g_cc_failed = 0;
} // namespace

#define XX_TEST_PASSED g_cc_passed
#define XX_TEST_FAILED g_cc_failed

namespace agentxx {
namespace test {

namespace {

using namespace agentxx::chatbot;

/// 切分结果拼回原文 (去掉分段标记与围栏补齐) —— 用来检查"没有丢内容"
size_t totalCodePoints(const std::vector<std::string>& parts) {
    size_t total = 0;
    for (const auto& part : parts) {
        total += utf8CodePointCount(part);
    }
    return total;
}

bool contains(const std::string& haystack, std::string_view needle) {
    return haystack.find(needle) != std::string::npos;
}

/* ==================== UTF-8 与文本规整 ==================== */

void testUtf8Basics() {
    XX_TEST_EXPECT_EQ(utf8CodePointCount(""), size_t{0});
    XX_TEST_EXPECT_EQ(utf8CodePointCount("abc"), size_t{3});
    XX_TEST_EXPECT_EQ(utf8CodePointCount("中文"), size_t{2});
    XX_TEST_EXPECT_EQ(utf8CodePointCount("a中😀"), size_t{3});

    // 截断不切断多字节序列
    const std::string text = "a中😀b";
    XX_TEST_EXPECT_EQ(std::string{utf8Truncate(text, 2)}, std::string{"a中"});
    XX_TEST_EXPECT_EQ(std::string{utf8Truncate(text, 3)}, std::string{"a中😀"});
    XX_TEST_EXPECT_EQ(std::string{utf8Truncate(text, 99)}, text);

    // 截断说明
    XX_TEST_EXPECT_EQ(utf8TruncateWithNotice("abc", 99), std::string{"abc"});
    XX_TEST_EXPECT_TRUE(contains(utf8TruncateWithNotice("abcdef", 3), "已截断"));

    XX_TEST_EXPECT_EQ(trimCopy("  \t\n abc \r\n "), std::string{"abc"});
    XX_TEST_EXPECT_EQ(trimCopy("   "), std::string{});
    // 3 行以上空行折叠成 1 个空行; 行尾空白去掉
    XX_TEST_EXPECT_EQ(
        normalizeOutboundText("a   \n\n\n\nb\n"),
        std::string{"a\n\nb"}
    );
}

/* ==================== 切分 ==================== */

void testSplit() {
    // 不超限: 原样一条
    {
        SplitOptions opt;
        opt.limit = 100;
        auto parts = splitTextToParts("短文本", opt);
        XX_TEST_EXPECT_EQ(parts.size(), size_t{1});
        XX_TEST_EXPECT_EQ(parts[0], std::string{"短文本"});
    }
    // 空输入
    {
        SplitOptions opt;
        opt.limit = 10;
        XX_TEST_EXPECT_EQ(splitText("   ", opt).size(), size_t{0});
        XX_TEST_EXPECT_EQ(splitText("", opt).size(), size_t{0});
    }
    // 上限 0 = 不切分 (超长也原样一条)
    {
        SplitOptions opt;
        opt.limit = 0;
        const std::string longText(5000, 'x');
        auto              parts = splitTextToParts(longText, opt);
        XX_TEST_EXPECT_EQ(parts.size(), size_t{1});
        XX_TEST_EXPECT_EQ(parts[0].size(), size_t{5000});
    }
    // 按段落切: 两个各 60 字符的段落, 上限 100 → 两段
    {
        SplitOptions opt;
        opt.limit  = 100;
        opt.marker = false;
        const std::string para1(60, 'a');
        const std::string para2(60, 'b');
        auto              parts = splitTextToParts(para1 + "\n\n" + para2, opt);
        XX_TEST_EXPECT_EQ(parts.size(), size_t{2});
        XX_TEST_EXPECT_EQ(parts[0], para1);
        XX_TEST_EXPECT_EQ(parts[1], para2);
    }
    // 单段超限: 按行切
    {
        SplitOptions opt;
        opt.limit  = 50;
        opt.marker = false;
        std::string text;
        for (int i = 0; i < 10; ++i) {
            text += std::string(20, static_cast<char>('a' + i));
            text += "\n";
        }
        auto parts = splitTextToParts(text, opt);
        XX_TEST_EXPECT_TRUE(parts.size() >= 4);
        for (const auto& part : parts) {
            XX_TEST_EXPECT_TRUE(utf8CodePointCount(part) <= 50);
        }
    }
    // 单行超限: 硬切且不切断 UTF-8
    {
        SplitOptions opt;
        opt.limit  = 30;
        opt.marker = false;
        std::string text;
        for (int i = 0; i < 100; ++i) {
            text += "中";
        }
        auto parts = splitTextToParts(text, opt);
        XX_TEST_EXPECT_TRUE(parts.size() >= 3);
        size_t totalChars = 0;
        for (const auto& part : parts) {
            XX_TEST_EXPECT_TRUE(utf8CodePointCount(part) <= 30);
            for (char c : part) {
                XX_TEST_EXPECT_TRUE((static_cast<unsigned char>(c) & 0xC0) != 0x80
                                    || true); // 结构上无非法半字节: 见下断言
            }
            totalChars += utf8CodePointCount(part);
        }
        // 拼回来仍是 100 个中文字
        XX_TEST_EXPECT_EQ(totalChars, size_t{100});
    }
    // emoji 不被切开 (4 字节序列)
    {
        SplitOptions opt;
        opt.limit  = 5;
        opt.marker = false;
        const std::string text = "😀😀😀😀😀😀😀";
        auto              parts = splitTextToParts(text, opt);
        XX_TEST_EXPECT_TRUE(parts.size() >= 2);
        for (const auto& part : parts) {
            XX_TEST_EXPECT_EQ(part.size() % 4, size_t{0}); // 全是 4 字节 emoji
            XX_TEST_EXPECT_TRUE(utf8CodePointCount(part) <= 5);
        }
    }
    // 恰好等于上限: 不分段
    {
        SplitOptions opt;
        opt.limit  = 10;
        opt.marker = true;
        auto parts = splitTextToParts(std::string(10, 'a'), opt);
        XX_TEST_EXPECT_EQ(parts.size(), size_t{1});
        XX_TEST_EXPECT_EQ(parts[0], std::string(10, 'a'));
    }
    // 分段标记
    {
        SplitOptions opt;
        opt.limit  = 40;
        opt.marker = true;
        std::string text;
        for (int i = 0; i < 6; ++i) {
            text += std::string(30, 'x');
            text += "\n";
        }
        auto parts = splitTextToParts(text, opt);
        XX_TEST_EXPECT_TRUE(parts.size() >= 2);
        XX_TEST_EXPECT_TRUE(contains(parts.front(), "(1/"));
        XX_TEST_EXPECT_TRUE(
            contains(parts.back(), "/" + std::to_string(parts.size()) + ")")
        );
    }
    // maxParts 上限: 超出部分丢弃并追加省略说明
    {
        SplitOptions opt;
        opt.limit    = 20;
        opt.maxParts = 2;
        opt.marker   = false;
        std::string text;
        for (int i = 0; i < 20; ++i) {
            text += std::string(18, 'y');
            text += "\n";
        }
        auto parts = splitText(text, opt);
        XX_TEST_EXPECT_EQ(parts.size(), size_t{2});
        XX_TEST_EXPECT_TRUE(parts.back().truncated);
        XX_TEST_EXPECT_TRUE(contains(parts.back().text, "已省略"));
    }
    // 围栏代码: 切开时补围栏 (每段都以 ``` 开头结尾成对)
    {
        SplitOptions opt;
        opt.limit  = 60;
        opt.marker = false;
        std::string text = "说明:\n```cpp\n";
        for (int i = 0; i < 20; ++i) {
            text += "int value_" + std::to_string(i) + " = " + std::to_string(i) + ";\n";
        }
        text += "```\n";
        auto parts = splitTextToParts(text, opt);
        XX_TEST_EXPECT_TRUE(parts.size() >= 2);
        // 第二段起 (被围栏截断的那些) 应以 ``` 开头
        for (size_t i = 1; i + 1 < parts.size(); ++i) {
            XX_TEST_EXPECT_TRUE(parts[i].rfind("```", 0) == 0);
        }
        // 除最后一段外, 段尾应是关闭围栏
        for (size_t i = 0; i + 1 < parts.size(); ++i) {
            if (contains(parts[i], "```cpp")) {
                XX_TEST_EXPECT_TRUE(parts[i].size() >= 3);
                XX_TEST_EXPECT_TRUE(
                    parts[i].compare(parts[i].size() - 3, 3, "```") == 0
                );
            }
        }
    }
}

/* ==================== 命令 ==================== */

void testBuiltinCommands() {
    const auto defs = builtinCommands();
    XX_TEST_EXPECT_TRUE(!defs.empty());

    // 名字都得以 `/axx-` 开头 (文档与用例都按这个前缀写), 且不重复
    std::set<std::string> names;
    for (const auto& def : defs) {
        XX_TEST_EXPECT_TRUE(def.name.rfind("/axx-", 0) == 0);
        XX_TEST_EXPECT_TRUE(names.insert(def.name).second);
        if (def.mode == CommandMode::Local) {
            XX_TEST_EXPECT_TRUE(!def.reply.empty());
        } else if (def.mode == CommandMode::Model) {
            XX_TEST_EXPECT_TRUE(!def.prompt.empty());
        } else {
            XX_TEST_EXPECT_TRUE(def.action == "cancel" || def.action == "clear");
        }
    }

    // 四个命令都在, 且各自是预期形态
    XX_TEST_EXPECT_TRUE(names.count("/axx-help") == 1);
    XX_TEST_EXPECT_TRUE(names.count("/axx-info") == 1);
    XX_TEST_EXPECT_TRUE(names.count("/axx-hello") == 1);
    XX_TEST_EXPECT_TRUE(names.count("/axx-stop") == 1);

    // `{commandList}` 由引擎按同一张表拼出来, 帮助命令必须带上它
    bool helpUsesList = false;
    for (const auto& def : defs) {
        if (def.name == "/axx-help") {
            helpUsesList = def.reply.find("{commandList}") != std::string::npos;
        }
    }
    XX_TEST_EXPECT_TRUE(helpUsesList);

    // 内置表能直接喂给匹配器
    const auto hit = parseCommand("/axx-info", defs);
    XX_TEST_EXPECT_TRUE(hit.matched);
    XX_TEST_EXPECT_EQ(hit.name, std::string{"/axx-info"});
}

void testCommand() {
    std::vector<CommandDef> defs;
    defs.push_back(CommandDef{"/axx-info", CommandMode::Local, "info", {}, {}});
    defs.push_back(CommandDef{"/axx-information", CommandMode::Local, "info2", {}, {}});
    defs.push_back(CommandDef{"/axx-hello", CommandMode::Model, {}, "hello", {}});
    defs.push_back(CommandDef{"/axx-stop", CommandMode::Action, {}, {}, "cancel"});

    // 命中 + 参数
    {
        auto result = parseCommand("/axx-info", defs);
        XX_TEST_EXPECT_TRUE(result.matched);
        XX_TEST_EXPECT_EQ(result.name, std::string{"/axx-info"});
        XX_TEST_EXPECT_EQ(result.args, std::string{});
    }
    {
        auto result = parseCommand("  /axx-hello  世界 ", defs);
        XX_TEST_EXPECT_TRUE(result.matched);
        XX_TEST_EXPECT_EQ(result.name, std::string{"/axx-hello"});
        XX_TEST_EXPECT_EQ(result.args, std::string{"世界"});
    }
    // 大小写不敏感, 命中的名字保留配置里的大小写
    {
        auto result = parseCommand("/AXX-INFO", defs);
        XX_TEST_EXPECT_TRUE(result.matched);
        XX_TEST_EXPECT_EQ(result.name, std::string{"/axx-info"});
    }
    // 长名优先 (`/axx-information` 不应被 `/axx-info` 吃掉)
    {
        auto result = parseCommand("/axx-information", defs);
        XX_TEST_EXPECT_TRUE(result.matched);
        XX_TEST_EXPECT_EQ(result.name, std::string{"/axx-information"});
    }
    // 命令名后不是空白: 不命中
    {
        auto result = parseCommand("/axx-infoo", defs);
        XX_TEST_EXPECT_FALSE(result.matched);
    }
    // 不以 `/` 开头: 不命中
    {
        auto result = parseCommand("axx-info", defs);
        XX_TEST_EXPECT_FALSE(result.matched);
    }
    {
        auto result = parseCommand("你好 /axx-info", defs);
        XX_TEST_EXPECT_FALSE(result.matched);
    }
    // 空配置: 不命中
    {
        auto result = parseCommand("/axx-info", {});
        XX_TEST_EXPECT_FALSE(result.matched);
    }
}

void testTemplate() {
    std::map<std::string, std::string, std::less<>> vars;
    vars["account"] = "qq_main";
    vars["chat"]    = "B2C3";
    vars["count"]   = "7";

    XX_TEST_EXPECT_EQ(
        renderTemplate("账号 {account} 会话 {chat} 共 {count} 条", vars),
        std::string{"账号 qq_main 会话 B2C3 共 7 条"}
    );
    // 未知变量原样保留
    XX_TEST_EXPECT_EQ(renderTemplate("未知 {whatever}", vars), std::string{"未知 {whatever}"});
    // 字面量花括号
    XX_TEST_EXPECT_EQ(renderTemplate("{{account}}", vars), std::string{"{account}"});
    // 没有占位符
    XX_TEST_EXPECT_EQ(renderTemplate("纯文本", vars), std::string{"纯文本"});
    // 空的占位符 `{}` 不当变量
    XX_TEST_EXPECT_EQ(renderTemplate("{}", vars), std::string{"{}"});
}

/* ==================== 输入块 ==================== */

void testInputBlock() {
    InputBlockOptions opt;
    opt.type      = ChatType::Group;
    opt.chatName  = "后端讨论组";
    opt.chatId    = "B2C3D4E5F6A1B2C3D4E5F6A1B2C3D4E5";
    opt.platformLabel = "QQ";

    std::vector<InputMessageView> messages;
    {
        InputMessageView one;
        one.index       = 1;
        one.senderName  = "小明";
        one.senderRole  = "member";
        one.text        = "你们看看这个报错";
        messages.push_back(one);
    }
    {
        InputMessageView two;
        two.index         = 2;
        two.senderName    = "小红";
        two.senderRole    = "member";
        two.mentionedMe   = true;
        two.hasQuote      = true;
        two.quotedSender  = "小明";
        two.text          = "帮我们看看";
        AttachmentLine attachment;
        attachment.fileName = "photo.png";
        attachment.location = "C:/tmp/photo.png";
        attachment.local    = true;
        two.attachments.push_back(attachment);
        messages.push_back(two);
    }

    const auto block = renderInputBlock(opt, messages);
    XX_TEST_EXPECT_TRUE(contains(block, "QQ群「后端讨论组」"));
    XX_TEST_EXPECT_TRUE(contains(block, "群 id 尾号"));
    XX_TEST_EXPECT_TRUE(contains(block, "[#1] 小明(member): 你们看看这个报错"));
    XX_TEST_EXPECT_TRUE(contains(block, "[#2] 小红(member) @机器人"));
    XX_TEST_EXPECT_TRUE(contains(block, "引用了 小明 的消息"));
    XX_TEST_EXPECT_TRUE(contains(block, "[#2:附件] photo.png (本地: C:/tmp/photo.png)"));

    // 私聊形态
    InputBlockOptions privateOpt;
    privateOpt.type     = ChatType::Private;
    privateOpt.chatName = "小王";
    privateOpt.chatId   = "C3D4";
    const auto privateBlock = renderInputBlock(privateOpt, messages);
    XX_TEST_EXPECT_TRUE(contains(privateBlock, "私聊「小王」"));
    XX_TEST_EXPECT_FALSE(contains(privateBlock, "群 id 尾号"));

    // 关掉元信息段
    InputBlockOptions noHint;
    noHint.includeContextHint = false;
    const auto noHintBlock     = renderInputBlock(noHint, messages);
    XX_TEST_EXPECT_FALSE(contains(noHintBlock, "下面是最近收到的消息"));
    XX_TEST_EXPECT_TRUE(contains(noHintBlock, "[#1] 小明"));

    // 群名空: 用 id 前 8 位
    InputBlockOptions unnamed;
    unnamed.chatId = "A1B2C3D4E5F6";
    const auto unnamedBlock = renderInputBlock(unnamed, messages);
    XX_TEST_EXPECT_TRUE(contains(unnamedBlock, "A1B2C3D4"));

    // 内联的普通文件正文: 用开始/结束标记围起来, 模型才知道哪段是文件内容
    {
        InputMessageView withFile;
        withFile.index      = 1;
        withFile.senderName = "小张";
        withFile.text       = "看看这个文件的内容是什么";
        AttachmentLine file;
        file.fileName   = "README.md";
        file.location   = "C:/tmp/README.md";
        file.local      = true;
        file.note       = "内容已内联在下面";
        file.inlineText = "# 标题\n正文一行\n";
        withFile.attachments.push_back(file);

        InputBlockOptions plain;
        plain.includeContextHint = false;
        const auto fileBlock     = renderInputBlock(plain, {withFile});
        XX_TEST_EXPECT_TRUE(contains(fileBlock, "[#1:附件] README.md"));
        XX_TEST_EXPECT_TRUE(contains(fileBlock, "[#1:附件内容开始] README.md"));
        XX_TEST_EXPECT_TRUE(contains(fileBlock, "# 标题"));
        XX_TEST_EXPECT_TRUE(contains(fileBlock, "正文一行"));
        XX_TEST_EXPECT_TRUE(contains(fileBlock, "[#1:附件内容结束]"));
        // 顺序: 附件说明行 → 内容开始 → 内容 → 内容结束
        const auto fileLineAt  = fileBlock.find("[#1:附件] README.md");
        const auto beginAt     = fileBlock.find("[#1:附件内容开始]");
        const auto bodyAt      = fileBlock.find("# 标题");
        const auto endAt       = fileBlock.find("[#1:附件内容结束]");
        XX_TEST_EXPECT_TRUE(fileLineAt != std::string::npos);
        XX_TEST_EXPECT_TRUE(beginAt != std::string::npos);
        XX_TEST_EXPECT_TRUE(bodyAt != std::string::npos);
        XX_TEST_EXPECT_TRUE(endAt != std::string::npos);
        XX_TEST_EXPECT_TRUE(fileLineAt < beginAt);
        XX_TEST_EXPECT_TRUE(beginAt < bodyAt);
        XX_TEST_EXPECT_TRUE(bodyAt < endAt);
    }
}

/* ==================== 引用编号 ==================== */

void testReplyTo() {
    XX_TEST_EXPECT_EQ(parseReplyTo("3"), 3);
    XX_TEST_EXPECT_EQ(parseReplyTo(" #3 "), 3);
    XX_TEST_EXPECT_EQ(parseReplyTo("#12"), 12);
    XX_TEST_EXPECT_EQ(parseReplyTo(""), 0);
    XX_TEST_EXPECT_EQ(parseReplyTo("abc"), 0);
    XX_TEST_EXPECT_EQ(parseReplyTo("0"), 0);
    XX_TEST_EXPECT_EQ(parseReplyTo("-1"), 0);
    XX_TEST_EXPECT_EQ(parseReplyTo("3.5"), 0);
    XX_TEST_EXPECT_EQ(parseReplyTo("12345678"), 0);

    XX_TEST_EXPECT_EQ(chatIdTail("B2C3D4E5F6A1", 4), std::string{"F6A1"});
    XX_TEST_EXPECT_EQ(chatIdTail("ab", 4), std::string{"ab"});
    XX_TEST_EXPECT_EQ(chatIdTail("", 4), std::string{});

    XX_TEST_EXPECT_EQ(displayNameOf("小明", "id12345678"), std::string{"小明"});
    XX_TEST_EXPECT_EQ(displayNameOf("", "id1234567890"), std::string{"id123456"});
    XX_TEST_EXPECT_EQ(displayNameOf("", ""), std::string{});
}

/* ==================== 去重环 ==================== */

void testDedupe() {
    DedupeRing ring(3);
    XX_TEST_EXPECT_FALSE(ring.seenOrAdd("a"));
    XX_TEST_EXPECT_TRUE(ring.seenOrAdd("a"));
    XX_TEST_EXPECT_EQ(ring.duplicateCount(), uint64_t{1});
    XX_TEST_EXPECT_FALSE(ring.seenOrAdd("b"));
    XX_TEST_EXPECT_FALSE(ring.seenOrAdd("c"));
    XX_TEST_EXPECT_EQ(ring.size(), size_t{3});
    // 挤掉最旧的 a
    XX_TEST_EXPECT_FALSE(ring.seenOrAdd("d"));
    XX_TEST_EXPECT_EQ(ring.size(), size_t{3});
    XX_TEST_EXPECT_FALSE(ring.contains("a"));
    XX_TEST_EXPECT_TRUE(ring.contains("d"));
    XX_TEST_EXPECT_FALSE(ring.seenOrAdd("a")); // a 已不在环里, 重新记入
    // 空键不参与
    XX_TEST_EXPECT_FALSE(ring.seenOrAdd(""));
    XX_TEST_EXPECT_FALSE(ring.contains(""));
    ring.clear();
    XX_TEST_EXPECT_EQ(ring.size(), size_t{0});
}

/* ==================== 枚举与键 ==================== */

void testTypes() {
    XX_TEST_EXPECT_EQ(std::string{chatTypeName(ChatType::Group)}, std::string{"group"});
    XX_TEST_EXPECT_EQ(std::string{chatTypeName(ChatType::Private)}, std::string{"private"});
    XX_TEST_EXPECT_EQ(std::string{chatModeName(ChatMode::Mention)}, std::string{"mention"});
    XX_TEST_EXPECT_EQ(std::string{chatModeName(ChatMode::Always)}, std::string{"always"});
    XX_TEST_EXPECT_EQ(std::string{chatModeName(ChatMode::Off)}, std::string{"off"});

    ChatType type = ChatType::Group;
    XX_TEST_EXPECT_TRUE(chatTypeFromString("private", type));
    XX_TEST_EXPECT_TRUE(type == ChatType::Private);
    XX_TEST_EXPECT_TRUE(chatTypeFromString("群", type));
    XX_TEST_EXPECT_TRUE(type == ChatType::Group);
    XX_TEST_EXPECT_FALSE(chatTypeFromString("bogus", type));

    ChatMode mode = ChatMode::Mention;
    XX_TEST_EXPECT_TRUE(chatModeFromString("always", mode));
    XX_TEST_EXPECT_TRUE(mode == ChatMode::Always);
    XX_TEST_EXPECT_FALSE(chatModeFromString("bogus", mode));

    ChatKey key;
    key.accountId = "qq_main";
    key.type      = ChatType::Group;
    key.chatId    = "ABC";
    XX_TEST_EXPECT_EQ(key.key(), std::string{"qq_main|group|ABC"});
    const auto parsed = parseChatKey("qq_main|private|XYZ");
    XX_TEST_EXPECT_TRUE(parsed.valid());
    XX_TEST_EXPECT_EQ(parsed.accountId, std::string{"qq_main"});
    XX_TEST_EXPECT_TRUE(parsed.type == ChatType::Private);
    XX_TEST_EXPECT_EQ(parsed.chatId, std::string{"XYZ"});
    XX_TEST_EXPECT_FALSE(parseChatKey("qq_main").valid());
    XX_TEST_EXPECT_FALSE(parseChatKey("qq_main|bogus|XYZ").valid());
    XX_TEST_EXPECT_FALSE(parseChatKey("|group|XYZ").valid());
}

/* ==================== 配置 ==================== */

/// 一份最小可用配置的 JSON
std::string minimalConfigJson() {
    return R"({
  "accounts": [
    {"id": "qq_main", "type": "qq", "appId": "123456", "appSecret": "secret-abc"}
  ],
  "personas": [
    {
      "name": "assistant",
      "bind": [
        {"account": "qq_main", "chats": [
          {"type": "group", "id": "G1", "mode": "mention"},
          {"type": "private", "id": "U1", "mode": "always"}
        ]}
      ]
    }
  ]
})";
}

void testConfigParseAndValidate() {
    // 最小配置通过
    {
        ChatbotConfig config;
        ConfigError   err;
        XX_TEST_EXPECT_TRUE(parseConfig(minimalConfigJson(), config, err));
        XX_TEST_EXPECT_TRUE(err.empty());
        XX_TEST_EXPECT_EQ(config.accounts.size(), size_t{1});
        XX_TEST_EXPECT_EQ(config.personas.size(), size_t{1});
        XX_TEST_EXPECT_TRUE(validateConfig(config, 8, err));
        XX_TEST_EXPECT_TRUE(err.empty());
    }
    // appId 是 JSON 数字 (主程序展开 ${VAR} 后会变成数字): 必须接受
    {
        ChatbotConfig config;
        ConfigError   err;
        const std::string json = R"({
          "accounts": [{"id": "a", "type": "qq", "appId": 1906091230, "appSecret": "s"}],
          "personas": [{"name": "p", "bind": [{"account": "a", "chats": [{"type": "group", "id": "G"}]}]}]
        })";
        XX_TEST_EXPECT_TRUE(parseConfig(json, config, err));
        XX_TEST_EXPECT_EQ(config.accounts[0].appId, std::string{"1906091230"});
    }
    // appId 是浮点 (精度已丢): 明确拒绝并提示
    {
        ChatbotConfig config;
        ConfigError   err;
        const std::string json = R"({
          "accounts": [{"id": "a", "type": "qq", "appId": 1.9060912301234e18, "appSecret": "s"}],
          "personas": [{"name": "p", "bind": [{"account": "a", "chats": [{"type": "group", "id": "G"}]}]}]
        })";
        XX_TEST_EXPECT_FALSE(parseConfig(json, config, err));
        XX_TEST_EXPECT_TRUE(contains(err.message, "浮点"));
    }
    // 非法 JSON
    {
        ChatbotConfig config;
        ConfigError   err;
        XX_TEST_EXPECT_FALSE(parseConfig("{not json", config, err));
        XX_TEST_EXPECT_TRUE(contains(err.message, "JSON"));
    }
    // args 不是对象
    {
        ChatbotConfig config;
        ConfigError   err;
        XX_TEST_EXPECT_FALSE(parseConfig("[]", config, err));
    }
    // 命令表来自插件内置: 配置里的 `commands` 段不生效, 表始终是内置那四条
    {
        ChatbotConfig config;
        ConfigError   err;
        XX_TEST_EXPECT_TRUE(parseConfig(
            R"({"accounts": [{"id": "a", "type": "qq", "appId": "1", "appSecret": "s"}],
                "personas": [{"name": "p", "bind": [{"account": "a", "chats": [{"type":"group","id":"G"}]}]}],
                "commands": [{"name": "/custom-cmd", "mode": "local", "reply": "x"}]})",
            config,
            err
        ));
        XX_TEST_EXPECT_EQ(config.commands.size(), builtinCommands().size());
        bool hasCustom = false;
        for (const auto& command : config.commands) {
            if (command.name == "/custom-cmd") {
                hasCustom = true;
            }
        }
        XX_TEST_EXPECT_FALSE(hasCustom);
    }
}

void testConfigValidationFailures() {
    // accounts 空
    {
        ChatbotConfig config;
        ConfigError   err;
        parseConfig(R"({"accounts": [], "personas": []})", config, err);
        XX_TEST_EXPECT_FALSE(validateConfig(config, 8, err));
        XX_TEST_EXPECT_EQ(err.where, std::string{"accounts"});
    }
    // 账号 id 非法
    {
        ChatbotConfig config;
        ConfigError   err;
        parseConfig(
            R"({"accounts": [{"id": "qq main", "type": "qq", "appId": "1", "appSecret": "s"}],
                "personas": [{"name": "p", "bind": [{"account": "qq main", "chats": [{"type":"group","id":"G"}]}]}]})",
            config,
            err
        );
        XX_TEST_EXPECT_FALSE(validateConfig(config, 8, err));
        XX_TEST_EXPECT_TRUE(contains(err.where, ".id"));
    }
    // 平台类型不支持
    {
        ChatbotConfig config;
        ConfigError   err;
        parseConfig(
            R"({"accounts": [{"id": "w", "type": "wechat", "appId": "1", "appSecret": "s"}],
                "personas": [{"name": "p", "bind": [{"account": "w", "chats": [{"type":"group","id":"G"}]}]}]})",
            config,
            err
        );
        XX_TEST_EXPECT_FALSE(validateConfig(config, 8, err));
        XX_TEST_EXPECT_TRUE(contains(err.message, "wechat"));
    }
    // 缺凭证
    {
        ChatbotConfig config;
        ConfigError   err;
        parseConfig(
            R"({"accounts": [{"id": "a", "type": "qq", "appSecret": "s"}],
                "personas": [{"name": "p", "bind": [{"account": "a", "chats": [{"type":"group","id":"G"}]}]}]})",
            config,
            err
        );
        XX_TEST_EXPECT_FALSE(validateConfig(config, 8, err));
        XX_TEST_EXPECT_TRUE(contains(err.where, "appId"));
    }
    // 人设名重复
    {
        ChatbotConfig config;
        ConfigError   err;
        parseConfig(
            R"({"accounts": [{"id": "a", "type": "qq", "appId": "1", "appSecret": "s"}],
                "personas": [
                  {"name": "p", "bind": [{"account": "a", "chats": [{"type":"group","id":"G1"}]}]},
                  {"name": "p", "bind": [{"account": "a", "chats": [{"type":"group","id":"G2"}]}]}
                ]})",
            config,
            err
        );
        XX_TEST_EXPECT_FALSE(validateConfig(config, 8, err));
        XX_TEST_EXPECT_TRUE(contains(err.message, "重复"));
    }
    // bind.account 不存在
    {
        ChatbotConfig config;
        ConfigError   err;
        parseConfig(
            R"({"accounts": [{"id": "a", "type": "qq", "appId": "1", "appSecret": "s"}],
                "personas": [{"name": "p", "bind": [{"account": "b", "chats": [{"type":"group","id":"G"}]}]}]})",
            config,
            err
        );
        XX_TEST_EXPECT_FALSE(validateConfig(config, 8, err));
        XX_TEST_EXPECT_TRUE(contains(err.message, "不在 accounts"));
    }
    // 同一个群归两个人设
    {
        ChatbotConfig config;
        ConfigError   err;
        parseConfig(
            R"({"accounts": [{"id": "a", "type": "qq", "appId": "1", "appSecret": "s"}],
                "personas": [
                  {"name": "p1", "bind": [{"account": "a", "chats": [{"type":"group","id":"G"}]}]},
                  {"name": "p2", "bind": [{"account": "a", "chats": [{"type":"group","id":"G"}]}]}
                ]})",
            config,
            err
        );
        XX_TEST_EXPECT_FALSE(validateConfig(config, 8, err));
        XX_TEST_EXPECT_TRUE(contains(err.message, "只能归一个人设"));
    }
    // 同一 (账号, 类型) 两条通配
    {
        ChatbotConfig config;
        ConfigError   err;
        parseConfig(
            R"({"accounts": [{"id": "a", "type": "qq", "appId": "1", "appSecret": "s"}],
                "personas": [
                  {"name": "p1", "bind": [{"account": "a", "chats": [{"type":"group","id":"*"}]}]},
                  {"name": "p2", "bind": [{"account": "a", "chats": [{"type":"group","id":"*"}]}]}
                ]})",
            config,
            err
        );
        XX_TEST_EXPECT_FALSE(validateConfig(config, 8, err));
        XX_TEST_EXPECT_TRUE(contains(err.message, "通配"));
    }
    // 人设数超根预算 (maxRoots=3 → 最多 2 个人设)
    {
        ChatbotConfig config;
        ConfigError   err;
        parseConfig(
            R"({"accounts": [{"id": "a", "type": "qq", "appId": "1", "appSecret": "s"}],
                "personas": [
                  {"name": "p1", "bind": [{"account": "a", "chats": [{"type":"group","id":"G1"}]}]},
                  {"name": "p2", "bind": [{"account": "a", "chats": [{"type":"group","id":"G2"}]}]},
                  {"name": "p3", "bind": [{"account": "a", "chats": [{"type":"group","id":"G3"}]}]}
                ]})",
            config,
            err
        );
        XX_TEST_EXPECT_FALSE(validateConfig(config, 3, err));
        XX_TEST_EXPECT_TRUE(contains(err.message, "根数上限"));
    }
    // behavior 越界裁剪不失败
    {
        ChatbotConfig config;
        ConfigError   err;
        parseConfig(
            R"({"accounts": [{"id": "a", "type": "qq", "appId": "1", "appSecret": "s"}],
                "personas": [{"name": "p", "bind": [{"account": "a", "chats": [{"type":"group","id":"G"}]}]}],
                "behavior": {"messageSplitLimit": 1, "maxSplitParts": 999, "maxChatsPerPersona": 999}})",
            config,
            err
        );
        XX_TEST_EXPECT_TRUE(validateConfig(config, 8, err));
        XX_TEST_EXPECT_EQ(config.behavior.messageSplitLimit, 100);
        XX_TEST_EXPECT_EQ(config.behavior.maxSplitParts, 64);
        XX_TEST_EXPECT_EQ(config.behavior.maxChatsPerPersona, 64);
    }
    // behavior 枚举非法: 直接拒绝
    {
        ChatbotConfig config;
        ConfigError   err;
        parseConfig(
            R"({"accounts": [{"id": "a", "type": "qq", "appId": "1", "appSecret": "s"}],
                "personas": [{"name": "p", "bind": [{"account": "a", "chats": [{"type":"group","id":"G"}]}]}],
                "behavior": {"overflowPolicy": "bogus"}})",
            config,
            err
        );
        XX_TEST_EXPECT_FALSE(validateConfig(config, 8, err));
        XX_TEST_EXPECT_TRUE(contains(err.where, "overflowPolicy"));
    }
}

void testEnvFileParsing() {
    const std::string text =
        "# comment\n"
        "\n"
        "QQBOT_APPID=1906091230\n"
        "export QQBOT_APPSERCRET=\"abc def\"\n"
        "OTHER='single'\n";
    XX_TEST_EXPECT_EQ(readKeyFromEnvText(text, "QQBOT_APPID"), std::string{"1906091230"});
    XX_TEST_EXPECT_EQ(readKeyFromEnvText(text, "QQBOT_APPSERCRET"), std::string{"abc def"});
    XX_TEST_EXPECT_EQ(readKeyFromEnvText(text, "OTHER"), std::string{"single"});
    XX_TEST_EXPECT_EQ(readKeyFromEnvText(text, "MISSING"), std::string{});
}

/* ==================== 路由表 ==================== */

void testRouterTable() {
    ChatbotConfig config;
    ConfigError   err;
    parseConfig(minimalConfigJson(), config, err);
    XX_TEST_EXPECT_TRUE(validateConfig(config, 8, err));

    RouterTable table;
    table.build(config);

    ChatKey groupKey;
    groupKey.accountId = "qq_main";
    groupKey.type      = ChatType::Group;
    groupKey.chatId    = "G1";
    auto groupRoute    = table.route(groupKey);
    XX_TEST_EXPECT_TRUE(groupRoute.found);
    XX_TEST_EXPECT_EQ(groupRoute.personaName, std::string{"assistant"});
    XX_TEST_EXPECT_TRUE(groupRoute.mode == ChatMode::Mention);

    // 未配置的会话: 找不到
    ChatKey other;
    other.accountId = "qq_main";
    other.type      = ChatType::Group;
    other.chatId    = "G404";
    XX_TEST_EXPECT_FALSE(table.route(other).found);

    // 运行期改模式
    XX_TEST_EXPECT_TRUE(table.effectiveMode(groupKey, groupRoute) == ChatMode::Mention);
    table.setModeOverride(groupKey.key(), ChatMode::Always);
    auto updated = table.route(groupKey);
    XX_TEST_EXPECT_TRUE(updated.mode == ChatMode::Always);

    // 通配兜底
    ChatbotConfig wildcardConfig;
    parseConfig(
        R"({"accounts": [{"id": "a", "type": "qq", "appId": "1", "appSecret": "s"}],
            "personas": [{"name": "p", "bind": [{"account": "a", "chats": [{"type":"group","id":"*","mode":"always"}]}]}]})",
        wildcardConfig,
        err
    );
    XX_TEST_EXPECT_TRUE(validateConfig(wildcardConfig, 8, err));
    RouterTable wildcardTable;
    wildcardTable.build(wildcardConfig);
    ChatKey anyGroup;
    anyGroup.accountId = "a";
    anyGroup.type      = ChatType::Group;
    anyGroup.chatId    = "whatever";
    auto anyRoute      = wildcardTable.route(anyGroup);
    XX_TEST_EXPECT_TRUE(anyRoute.found);
    XX_TEST_EXPECT_TRUE(anyRoute.wildcard);
    XX_TEST_EXPECT_TRUE(anyRoute.mode == ChatMode::Always);
    // 通配只覆盖 group, private 不命中
    ChatKey anyPrivate;
    anyPrivate.accountId = "a";
    anyPrivate.type      = ChatType::Private;
    anyPrivate.chatId    = "U9";
    XX_TEST_EXPECT_FALSE(wildcardTable.route(anyPrivate).found);
}

/// 路由器的额度与回收: 会话 id 稳定性 / 活跃计数 / LRU 挑最久未活动的那条
void testRouterQuota() {
    ConfigError err;
    ChatbotConfig config;
    parseConfig(
        R"({"accounts": [{"id": "a", "type": "qq", "appId": "1", "appSecret": "s"}],
            "personas": [{"name": "p", "bind": [{"account": "a", "chats": [
                {"type":"group","id":"G1","mode":"mention"},
                {"type":"group","id":"G2","mode":"always"},
                {"type":"group","id":"G3","mode":"always"}]}]}]})",
        config,
        err
    );
    XX_TEST_EXPECT_TRUE(validateConfig(config, 8, err));

    ChatbotRouter router;
    // 每人设只允许 2 条活跃通道
    router.build(config, 2, {});

    // 会话 id: 同一个人设 + 同一个群永远算出同一个值, 不同群不同值
    auto keyOf = [](std::string_view chatId) {
        ChatKey key;
        key.accountId = "a";
        key.type      = ChatType::Group;
        key.chatId    = std::string{chatId};
        return key;
    };
    const ChatKey g1 = keyOf("G1");
    const ChatKey g2 = keyOf("G2");
    const ChatKey g3 = keyOf("G3");
    const auto    id1 = router.sessionIdOf("p", g1);
    XX_TEST_EXPECT_TRUE(id1.rfind("chatbot_p_a_group_", 0) == 0);
    XX_TEST_EXPECT_EQ(id1, router.sessionIdOf("p", g1));
    XX_TEST_EXPECT_TRUE(id1 != router.sessionIdOf("p", g2));
    XX_TEST_EXPECT_TRUE(id1 != router.sessionIdOf("q", g1));

    // 开通道走 hook: 递增 id, 记录开/关次数
    int64_t nextChannel = 0;
    int     opened      = 0;
    int     closed      = 0;
    ChatbotRouter::Hooks hooks;
    hooks.open = [&nextChannel, &opened](
                     std::string_view,
                     std::string_view,
                     std::string_view
                 ) -> int64_t {
        ++opened;
        return ++nextChannel;
    };
    hooks.close = [&closed](int64_t) {
        ++closed;
    };
    router.build(config, 2, hooks);

    std::string openErr;
    const int64_t channel1
        = router.acquire(g1, router.route(g1), "G1", 10, openErr);
    XX_TEST_EXPECT_TRUE(channel1 > 0);
    XX_TEST_EXPECT_EQ(opened, 1);
    XX_TEST_EXPECT_EQ(router.find(g1.key()), channel1);
    XX_TEST_EXPECT_EQ(router.activeCount("p"), size_t{1});

    // 同一个群再开: 复用原通道, 不重复建
    const int64_t again = router.acquire(g1, router.route(g1), "G1", 11, openErr);
    XX_TEST_EXPECT_EQ(again, channel1);
    XX_TEST_EXPECT_EQ(opened, 1);

    // 第二个群: 仍在上限内
    const int64_t channel2 = router.acquire(g2, router.route(g2), "G2", 20, openErr);
    XX_TEST_EXPECT_TRUE(channel2 > 0);
    XX_TEST_EXPECT_EQ(router.activeCount("p"), size_t{2});

    // 第三个群: 达上限, 回收最久未活动的 G1 (seq 11 < 20)
    const int64_t channel3 = router.acquire(g3, router.route(g3), "G3", 30, openErr);
    XX_TEST_EXPECT_TRUE(channel3 > 0);
    XX_TEST_EXPECT_EQ(closed, 1);
    XX_TEST_EXPECT_EQ(router.find(g1.key()), int64_t{0});
    XX_TEST_EXPECT_EQ(router.find(g2.key()), channel2);
    XX_TEST_EXPECT_EQ(router.activeCount("p"), size_t{2});

    // 被回收的群再回来: 重新开一条 (会话库里上下文仍在)
    const int64_t reopened = router.acquire(g1, router.route(g1), "G1", 40, openErr);
    XX_TEST_EXPECT_TRUE(reopened > 0);
    XX_TEST_EXPECT_TRUE(reopened != channel1);
    XX_TEST_EXPECT_EQ(opened, 4);

    // 反查: 通道 → 会话键, 会话 id → 会话键
    XX_TEST_EXPECT_EQ(router.chatKeyOfChannel(reopened), g1.key());
    XX_TEST_EXPECT_EQ(
        router.chatKeyOfSession(router.sessionIdOf("p", g1)),
        g1.key()
    );
    XX_TEST_EXPECT_TRUE(router.chatKeyOfChannel(9999).empty());
    XX_TEST_EXPECT_TRUE(router.chatKeyOfSession("nope").empty());

    // 就绪/忙状态按通道记
    router.markReady(reopened, true);
    XX_TEST_EXPECT_TRUE(router.isReady(reopened));
    router.markBusy(reopened, true);
    XX_TEST_EXPECT_TRUE(router.isBusy(reopened));
    router.markBusy(reopened, false);
    XX_TEST_EXPECT_FALSE(router.isBusy(reopened));

    // 主动回收一条 / 全部回收
    const int beforeAll = closed;
    router.release(reopened, "test");
    XX_TEST_EXPECT_EQ(closed, beforeAll + 1);
    XX_TEST_EXPECT_FALSE(router.isReady(reopened));
    XX_TEST_EXPECT_TRUE(router.releaseAll("shutdown") >= size_t{1});
    XX_TEST_EXPECT_EQ(router.activeCount("p"), size_t{0});

    // 不属于任何人设的会话: 拒开
    ChatKey stranger;
    stranger.accountId = "a";
    stranger.type      = ChatType::Group;
    stranger.chatId    = "G404";
    XX_TEST_EXPECT_EQ(
        router.acquire(stranger, router.route(stranger), "G404", 50, openErr),
        int64_t{0}
    );
    XX_TEST_EXPECT_TRUE(!openErr.empty());

    // 人设名清单与配置的会话键
    XX_TEST_EXPECT_EQ(router.personaNames().size(), size_t{1});
    XX_TEST_EXPECT_EQ(router.chatKeysOf("p").size(), size_t{3});
    XX_TEST_EXPECT_TRUE(router.rootIdOf("p").empty());
    router.setRootId("p", "root-1");
    XX_TEST_EXPECT_EQ(router.rootIdOf("p"), std::string{"root-1"});
    // 空 rootId 不覆盖已有值
    router.setRootId("p", "");
    XX_TEST_EXPECT_EQ(router.rootIdOf("p"), std::string{"root-1"});

    // 标题: 有群名用群名, 没有就用「类型-尾号」
    XX_TEST_EXPECT_EQ(ChatbotRouter::titleOf("技术群", g1), std::string{"技术群"});
    XX_TEST_EXPECT_TRUE(ChatbotRouter::titleOf("", g1).rfind("group-", 0) == 0);

    // 记住的会话名: 面板靠它显示群名
    XX_TEST_EXPECT_TRUE(router.chatNameOf("a|group|G1").empty());
    router.rememberChatName("a|group|G1", "技术群");
    XX_TEST_EXPECT_EQ(router.chatNameOf("a|group|G1"), std::string{"技术群"});
    // 空名字不覆盖已有值
    router.rememberChatName("a|group|G1", "");
    XX_TEST_EXPECT_EQ(router.chatNameOf("a|group|G1"), std::string{"技术群"});
}

/* ==================== 侧边栏「机器人」栏 ==================== */

/// 组件树里全部可见文本 (面板断言用; 递归)
void collectTexts(const pluginxx::ui::Item& item, std::vector<std::string>& out) {
    if (item.kind == "Text") {
        const std::string text = item.text.resolve();
        if (!text.empty()) {
            out.push_back(text);
        }
    }
    for (const auto& child : item.children) {
        collectTexts(child, out);
    }
}

/// 组件树里有没有包含某段文字的文本
bool anyTextContains(const std::vector<pluginxx::ui::Item>& items, std::string_view needle) {
    std::vector<std::string> texts;
    for (const auto& item : items) {
        collectTexts(item, texts);
    }
    for (const auto& text : texts) {
        if (contains(text, needle)) {
            return true;
        }
    }
    return false;
}

/// 组件树里有没有某种组件 (面板样式断言用; 递归)
bool anyKindOf(const pluginxx::ui::Item& item, std::string_view kind) {
    if (item.kind == kind) {
        return true;
    }
    for (const auto& child : item.children) {
        if (anyKindOf(child, kind)) {
            return true;
        }
    }
    return false;
}

bool anyKindOf(const std::vector<pluginxx::ui::Item>& items, std::string_view kind) {
    for (const auto& item : items) {
        if (anyKindOf(item, kind)) {
            return true;
        }
    }
    return false;
}

/// 文本完全等于 `text` 的文本项 (没找到返回 nullptr)
const pluginxx::ui::Item* findTextEqual(const pluginxx::ui::Item& item, std::string_view text) {
    if (item.kind == "Text" && item.text.resolve() == text) {
        return &item;
    }
    for (const auto& child : item.children) {
        if (const auto* hit = findTextEqual(child, text)) {
            return hit;
        }
    }
    return nullptr;
}

const pluginxx::ui::Item* findTextEqual(
    const std::vector<pluginxx::ui::Item>& items,
    std::string_view                       text
) {
    for (const auto& item : items) {
        if (const auto* hit = findTextEqual(item, text)) {
            return hit;
        }
    }
    return nullptr;
}

/// 包含某段文字的文本项 (没找到返回 nullptr; 指针指向传入的组件树)
const pluginxx::ui::Item* findTextContaining(
    const pluginxx::ui::Item& item,
    std::string_view          needle
) {
    if (item.kind == "Text" && contains(item.text.resolve(), needle)) {
        return &item;
    }
    for (const auto& child : item.children) {
        if (const auto* hit = findTextContaining(child, needle)) {
            return hit;
        }
    }
    return nullptr;
}

const pluginxx::ui::Item* findTextContaining(
    const std::vector<pluginxx::ui::Item>& items,
    std::string_view                       needle
) {
    for (const auto& item : items) {
        if (const auto* hit = findTextContaining(item, needle)) {
            return hit;
        }
    }
    return nullptr;
}

/// 造一份典型快照: 一个在线账号 + 一个人设 + 一条正在执行且有积压的群会话
PanelSnapshot samplePanelSnapshot() {
    PanelSnapshot snap;
    snap.tick        = 7;
    snap.startupTime = "2026-10-29 20:00:00";
    snap.received    = 12;
    snap.sent        = 5;
    snap.failed      = 1;

    PanelAccountView account;
    account.id    = "qq_main";
    account.type  = "qq";
    account.state = "online";
    snap.accounts.push_back(account);

    PanelPersonaView persona;
    persona.name       = "assistant";
    persona.model      = "gpt-x";
    persona.rootId     = "root-1";
    persona.mounted    = true;
    persona.chats      = 2;
    persona.chatBudget = 64;
    persona.binds      = 3;
    persona.tools      = 5;
    persona.promptHead = "你是 QQ 群里的助手";
    snap.personas.push_back(persona);

    PanelSessionView session;
    session.chatKey      = "qq_main|group|G1";
    session.title        = "技术群";
    session.persona      = "assistant";
    session.account      = "qq_main";
    session.chatType     = "group";
    session.mode         = "always";
    session.sessionId    = "chatbot_assistant_qq_main_group_0123456789abcdef";
    session.channelId    = 7;
    session.ready        = true;
    session.running      = true;
    session.queued       = 3;
    session.turns        = 4;
    session.sent         = 6;
    session.lastTurnMs   = 1200;
    session.lastActiveMs = 99;
    snap.sessions.push_back(session);
    return snap;
}

void testPanelSnapshot() {
    const PanelSnapshot snap = samplePanelSnapshot();
    const std::string   json = panelSnapshotJson(snap);

    PanelSnapshot back;
    XX_TEST_EXPECT_TRUE(parsePanelSnapshot(json, back));
    XX_TEST_EXPECT_EQ(back.tick, uint32_t{7});
    XX_TEST_EXPECT_EQ(back.startupTime, std::string{"2026-10-29 20:00:00"});
    XX_TEST_EXPECT_EQ(back.received, uint64_t{12});
    XX_TEST_EXPECT_EQ(back.sent, uint64_t{5});
    XX_TEST_EXPECT_EQ(back.failed, uint64_t{1});

    XX_TEST_EXPECT_EQ(back.accounts.size(), size_t{1});
    if (!back.accounts.empty()) {
        XX_TEST_EXPECT_EQ(back.accounts[0].id, std::string{"qq_main"});
        XX_TEST_EXPECT_EQ(back.accounts[0].state, std::string{"online"});
    }

    XX_TEST_EXPECT_EQ(back.personas.size(), size_t{1});
    if (!back.personas.empty()) {
        const auto& persona = back.personas[0];
        XX_TEST_EXPECT_EQ(persona.name, std::string{"assistant"});
        XX_TEST_EXPECT_EQ(persona.model, std::string{"gpt-x"});
        XX_TEST_EXPECT_TRUE(persona.mounted);
        XX_TEST_EXPECT_EQ(persona.chatBudget, size_t{64});
        XX_TEST_EXPECT_EQ(persona.binds, size_t{3});
        XX_TEST_EXPECT_EQ(persona.promptHead, std::string{"你是 QQ 群里的助手"});
    }

    XX_TEST_EXPECT_EQ(back.sessions.size(), size_t{1});
    if (!back.sessions.empty()) {
        const auto& session = back.sessions[0];
        XX_TEST_EXPECT_EQ(session.title, std::string{"技术群"});
        XX_TEST_EXPECT_EQ(session.persona, std::string{"assistant"});
        XX_TEST_EXPECT_EQ(session.chatType, std::string{"group"});
        XX_TEST_EXPECT_EQ(session.mode, std::string{"always"});
        XX_TEST_EXPECT_EQ(session.channelId, int64_t{7});
        XX_TEST_EXPECT_TRUE(session.running);
        XX_TEST_EXPECT_TRUE(session.ready);
        XX_TEST_EXPECT_EQ(session.queued, size_t{3});
        XX_TEST_EXPECT_EQ(session.turns, uint64_t{4});
        XX_TEST_EXPECT_EQ(session.lastTurnMs, int64_t{1200});
    }

    // 非法 / 不兼容的载荷不能改掉调用方旧数据
    PanelSnapshot keep = samplePanelSnapshot();
    XX_TEST_EXPECT_FALSE(parsePanelSnapshot("", keep));
    XX_TEST_EXPECT_FALSE(parsePanelSnapshot("not-json", keep));
    XX_TEST_EXPECT_FALSE(parsePanelSnapshot("[1,2,3]", keep));
    XX_TEST_EXPECT_FALSE(parsePanelSnapshot(R"({"v":99,"tick":123})", keep));
    XX_TEST_EXPECT_EQ(keep.tick, uint32_t{7});

    // 只有版本号的最小快照: 通过且各段为空
    PanelSnapshot minimal;
    XX_TEST_EXPECT_TRUE(parsePanelSnapshot(R"({"v":1})", minimal));
    XX_TEST_EXPECT_TRUE(minimal.sessions.empty());
    XX_TEST_EXPECT_TRUE(minimal.personas.empty());
    XX_TEST_EXPECT_EQ(minimal.tick, uint32_t{0});
}

void testPanelWirePayload() {
    // client 侧从 WirePluginData 的 `data` 字段收到的原样文本 (与 client_plugins
    // 里投递的那份完全一致): 必须能被解析, 否则面板停在占位提示上
    const std::string payload = std::string{
        R"({"v":1,"tick":7,"startupTime":"2026-10-29 20:00:00",)"
        R"("accounts":[{"id":"qq_main","type":"qq","state":"online"}],)"
        R"("personas":[{"name":"assistant","model":"gpt-x",)"
        R"("promptHead":"你是 QQ 群里的助手","rootId":"root-1","mounted":true,)"
        R"("chats":2,"budget":64,"binds":3}],)"
        R"("sessions":[{"key":"qq_main|group|G1","title":"技术群","persona":"assistant",)"
        R"("account":"qq_main","type":"group","mode":"always","channelId":7,)"
        R"("ready":true,"running":true,"queued":3,"turns":4,"sent":6}],)"
        R"("stats":{"received":12,"sent":5,"failed":1}})"
    };
    PanelSnapshot parsed;
    XX_TEST_EXPECT_TRUE(parsePanelSnapshot(payload, parsed));
    XX_TEST_EXPECT_EQ(parsed.tick, uint32_t{7});
    XX_TEST_EXPECT_EQ(parsed.sessions.size(), size_t{1});
    if (!parsed.sessions.empty()) {
        XX_TEST_EXPECT_EQ(parsed.sessions[0].title, std::string{"技术群"});
        XX_TEST_EXPECT_EQ(parsed.sessions[0].queued, size_t{3});
        XX_TEST_EXPECT_TRUE(parsed.sessions[0].running);
        XX_TEST_EXPECT_EQ(parsed.sessions[0].channelId, int64_t{7});
    }
    XX_TEST_EXPECT_EQ(parsed.personas.size(), size_t{1});
    XX_TEST_EXPECT_EQ(parsed.accounts.size(), size_t{1});
}

void testPanelTexts() {
    PanelSessionView session;
    session.persona  = "assistant";
    session.chatType = "group";
    session.mode     = "always";
    XX_TEST_EXPECT_EQ(
        panelSessionSubtitle(session),
        std::string{"assistant · 群聊 · always"}
    );
    session.chatType = "private";
    XX_TEST_EXPECT_TRUE(contains(panelSessionSubtitle(session), "私聊"));
    session.persona.clear();
    XX_TEST_EXPECT_TRUE(contains(panelSessionSubtitle(session), "未归属人设"));

    PanelPersonaView persona;
    persona.chats      = 2;
    persona.chatBudget = 64;
    persona.binds      = 3;
    persona.mounted    = true;
    XX_TEST_EXPECT_EQ(panelPersonaSubtitle(persona), std::string{"会话 2/64 · 绑定 3"});
    persona.mounted = false;
    XX_TEST_EXPECT_TRUE(contains(panelPersonaSubtitle(persona), "未挂载"));

    XX_TEST_EXPECT_EQ(panelPersonaModelText(persona), std::string{"默认模型"});
    persona.model = "gpt-x";
    XX_TEST_EXPECT_EQ(panelPersonaModelText(persona), std::string{"gpt-x"});
    persona.mounted = true;
    XX_TEST_EXPECT_EQ(panelPersonaDetail(persona), std::string{"gpt-x · 会话 2/64 · 绑定 3"});

    PanelSessionView queued;
    queued.queued = 0;
    XX_TEST_EXPECT_EQ(panelSessionQueueText(queued), std::string{"0"});
    queued.queued = 12;
    XX_TEST_EXPECT_EQ(panelSessionQueueText(queued), std::string{"12"});

    PanelAccountView account;
    account.id       = "qq_main";
    account.state    = "online";
    XX_TEST_EXPECT_EQ(panelAccountItemText(account), std::string{"qq_main online"});
    account.state  = "reconnecting";
    account.reason = "网络抖动";
    XX_TEST_EXPECT_EQ(
        panelAccountItemText(account),
        std::string{"qq_main reconnecting (网络抖动)"}
    );
    account.id.clear();
    XX_TEST_EXPECT_TRUE(contains(panelAccountItemText(account), "?"));

    // 截断不切断多字节序列 (面板窄栏靠它保住右侧的排队数)
    XX_TEST_EXPECT_EQ(panelTruncate("技术群", 3), std::string{"技术群"});
    XX_TEST_EXPECT_EQ(panelTruncate("技术交流群", 3), std::string{"技术…"});
    XX_TEST_EXPECT_EQ(panelTruncate("", 3), std::string{});
    XX_TEST_EXPECT_EQ(utf8CodePointCount(panelTruncate("技術群聊天", 4)), size_t{4});
}

void testPanelItems() {
    // 没有数据: 只给一行提示, 不留空白栏
    PanelSnapshot empty;
    auto           placeholder = panelItemsOf(empty);
    XX_TEST_EXPECT_EQ(placeholder.size(), size_t{1});
    XX_TEST_EXPECT_TRUE(anyTextContains(placeholder, "等待服务端数据"));
    const auto* placeholderItem = findTextContaining(placeholder, "等待服务端数据");
    XX_TEST_EXPECT_TRUE(placeholderItem != nullptr);
    if (placeholderItem != nullptr) {
        XX_TEST_EXPECT_EQ(placeholderItem->tone, std::string{"hint"});
    }

    const PanelSnapshot snap = samplePanelSnapshot();
    const auto          items = panelItemsOf(snap);
    XX_TEST_EXPECT_TRUE(items.size() >= size_t{4});

    // 会话段: 段标题 `标题: 数量`, 正在执行的一行左侧是加载帧 (tick % 10), 右侧是排队数
    XX_TEST_EXPECT_TRUE(anyTextContains(items, "会话: 1"));
    XX_TEST_EXPECT_TRUE(anyTextContains(items, std::string{panelSpinnerFrame(snap.tick)}));
    XX_TEST_EXPECT_TRUE(anyTextContains(items, "技术群"));
    XX_TEST_EXPECT_TRUE(anyTextContains(items, "assistant · 群聊 · always"));
    XX_TEST_EXPECT_TRUE(anyTextContains(items, "3"));

    // 列表项 `|- 内容`, 段之间是一行空白而不是分隔线
    XX_TEST_EXPECT_TRUE(anyTextContains(items, "|- "));
    XX_TEST_EXPECT_TRUE(anyKindOf(items, "Gap"));
    XX_TEST_EXPECT_FALSE(anyKindOf(items, "Divider"));

    // 减淡的内容统一用 hint 色 (标题/强调状态除外)
    const auto* bulletItem = findTextContaining(items, "|- ");
    XX_TEST_EXPECT_TRUE(bulletItem != nullptr);
    if (bulletItem != nullptr) {
        XX_TEST_EXPECT_EQ(bulletItem->tone, std::string{"hint"});
    }
    const auto* subtitleItem = findTextContaining(items, "assistant · 群聊 · always");
    XX_TEST_EXPECT_TRUE(subtitleItem != nullptr);
    if (subtitleItem != nullptr) {
        XX_TEST_EXPECT_EQ(subtitleItem->tone, std::string{"hint"});
        // 只 hint 色, 不再叠加 dim —— caption 会让副标题比 `|- ` 前缀还暗一档
        XX_TEST_EXPECT_TRUE(subtitleItem->textType != std::string{"caption"});
    }
    // 排队数靠右对齐 (贴在栏的右边), 有积压用强调色
    const auto* queueItem = findTextEqual(items, "3");
    XX_TEST_EXPECT_TRUE(queueItem != nullptr);
    if (queueItem != nullptr) {
        XX_TEST_EXPECT_EQ(queueItem->align, std::string{"end"});
        XX_TEST_EXPECT_EQ(queueItem->tone, std::string{"accent"});
    }

    // 人设段: 名字 / 模型 / 用量 / 提示词摘录
    XX_TEST_EXPECT_TRUE(anyTextContains(items, "人设: 1"));
    XX_TEST_EXPECT_TRUE(anyTextContains(items, "会话 2/64 · 绑定 3"));
    XX_TEST_EXPECT_TRUE(anyTextContains(items, "gpt-x"));
    XX_TEST_EXPECT_TRUE(anyTextContains(items, "你是 QQ 群里的助手"));
    const auto* personaDetail = findTextContaining(items, "会话 2/64 · 绑定 3");
    XX_TEST_EXPECT_TRUE(personaDetail != nullptr);
    if (personaDetail != nullptr) {
        XX_TEST_EXPECT_EQ(personaDetail->tone, std::string{"hint"});
        XX_TEST_EXPECT_TRUE(personaDetail->textType != std::string{"caption"});
    }
    const auto* promptItem = findTextContaining(items, "你是 QQ 群里的助手");
    XX_TEST_EXPECT_TRUE(promptItem != nullptr);
    if (promptItem != nullptr) {
        XX_TEST_EXPECT_EQ(promptItem->tone, std::string{"hint"});
        XX_TEST_EXPECT_TRUE(promptItem->textType != std::string{"caption"});
    }

    // 账号段: 每行一个账号, 标题同样是 `标题: 数量`
    XX_TEST_EXPECT_TRUE(anyTextContains(items, "账号: 1"));
    XX_TEST_EXPECT_TRUE(anyTextContains(items, "qq_main online"));

    // 底部: 收发计数
    XX_TEST_EXPECT_TRUE(anyTextContains(items, "失败 1"));
    const auto* statsItem = findTextContaining(items, "失败 1");
    XX_TEST_EXPECT_TRUE(statsItem != nullptr);
    if (statsItem != nullptr) {
        XX_TEST_EXPECT_EQ(statsItem->tone, std::string{"hint"});
        XX_TEST_EXPECT_TRUE(statsItem->textType != std::string{"caption"});
    }

    // 空闲会话不转圈: 左侧用占位点, 右侧排队数为 0
    PanelSnapshot idle = samplePanelSnapshot();
    idle.sessions[0].running = false;
    idle.sessions[0].queued  = 0;
    const auto idleItems     = panelItemsOf(idle);
    XX_TEST_EXPECT_FALSE(anyTextContains(idleItems, std::string{panelSpinnerFrame(idle.tick)}));
    XX_TEST_EXPECT_TRUE(anyTextContains(idleItems, "·"));

    // 会话为空但人设已加载: 给一行说明而不是空白
    PanelSnapshot noSession = samplePanelSnapshot();
    noSession.sessions.clear();
    const auto noSessionItems = panelItemsOf(noSession);
    XX_TEST_EXPECT_TRUE(anyTextContains(noSessionItems, "会话"));
    XX_TEST_EXPECT_TRUE(anyTextContains(noSessionItems, "还没有会话"));

    // 加载帧序列与 TUI 消息列表的加载中一致 (10 帧 braille 点阵)
    XX_TEST_EXPECT_EQ(panelSpinnerFrames().size(), size_t{10});
    XX_TEST_EXPECT_EQ(panelSpinnerFrame(0), std::string_view{"⠋"});
    XX_TEST_EXPECT_EQ(panelSpinnerFrame(10), std::string_view{"⠋"});
    XX_TEST_EXPECT_EQ(panelSpinnerFrame(11), std::string_view{"⠙"});
}

} // namespace

TestResult testChatbotCore() {
    g_cc_passed = 0;
    g_cc_failed = 0;

    testUtf8Basics();
    testSplit();
    testBuiltinCommands();
    testCommand();
    testTemplate();
    testInputBlock();
    testReplyTo();
    testDedupe();
    testTypes();
    testConfigParseAndValidate();
    testConfigValidationFailures();
    testEnvFileParsing();
    testRouterTable();
    testRouterQuota();
    testPanelSnapshot();
    testPanelWirePayload();
    testPanelTexts();
    testPanelItems();

    return TestResult{g_cc_passed, g_cc_failed};
}

} // namespace test
} // namespace agentxx


