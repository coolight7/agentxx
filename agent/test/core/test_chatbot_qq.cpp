///
/// `chatbot_qq` —— QQ 网关事件 → `InboundEvent` 的归一化回归
///
/// 为什么单独一层用例: 平台对不同附件 (图片 / 语音 / 文件 / 引用 / 卡片) 的字段布局
/// 并不一致, 只按文档猜会整类漏掉。下面的 JSON 全部来自**真机联调日志**
/// (`resource/history/chatbot/work.md`), 以后平台改字段时这里会直接失败。
///
/// 两个曾经踩过的坑 (都有对应用例):
/// - 群里"引用一个文件问它是什么": 文件只在 `msg_elements[0].attachments` 里,
///   只读 `msg_elements[0].content` (空串) 就永远读不到文件;
/// - `message_scene.ext` 既可能是字符串也可能是**字符串数组**, 按字符串取会拿到空值,
///   于是引用回复 (`replyRef`) 一直是空的。
#include "agentxx-test/core/test_chatbot_qq.h"

#include "agentxx_chatbot/chatbot_types.h"
#include "agentxx_chatbot/platform/qq/qq_normalize.h"

#include "utilxx_base/json.h"

#include <string>

namespace {
int g_cq_passed = 0;
int g_cq_failed = 0;
} // namespace

#define XX_TEST_PASSED g_cq_passed
#define XX_TEST_FAILED g_cq_failed

namespace agentxx {
namespace test {

namespace {

using namespace agentxx::chatbot;
using utilxx_base::Json;

/// 真机抓到的事件: 群里先发一个文件, 再引用它 @机器人 问"这里写了什么"
///
/// 关键点 (都被下面的用例逐条断言):
/// - `content` = 用户打的那句话
/// - `msg_elements[0].content` = 空串 (被引用的是文件消息, 没有正文)
/// - `msg_elements[0].attachments[0]` = 文件本体 (content_type `file` + url)
/// - `message_scene.ext` = 字符串数组, 里面有 `msg_idx` / `ref_msg_idx`
constexpr const char* kRealQuotedFileEvent = R"JSON({
  "author": {
    "bot": false,
    "id": "BFD86A0EA982C29C0ACC9F1F0EA38E00",
    "member_openid": "BFD86A0EA982C29C0ACC9F1F0EA38E00",
    "member_role": "owner",
    "union_openid": "",
    "username": "寒光"
  },
  "content": "  看看这个文件的内容是什么",
  "group_id": "0D5F5A2F37EFDF78CD830538C4D1B0F8",
  "group_openid": "0D5F5A2F37EFDF78CD830538C4D1B0F8",
  "id": "ROBOT1.0_wMrMAGr.rxyLNQbBodHI37N8pEsemk8CZXX0HkpjWcM793Z8iIueGjq.46T0JX782XQfvP5f91bjRAHuii6FdR3veRQN2U8PTdvzika8BXvPlNcCu0N7N5SmoOzGYJpn",
  "message_scene": {
    "ext": [
      "ref_msg_idx=REFIDX_SX8TGkckVZgyYb3U3Hoxp73JzqGDQodXzUPhygTQAZSiBmKxPn4YqesVn9xJz3Jjh+yQEXAm4j9JyrpoZxcAshoAIcPEZICA2ihJ/QyyJpTPrYhbOKFX2dK20eMBgQln",
      "msg_idx=REFIDX_w8FJB+sgWPQTyqKSZV1/R73JzqGDQodXzUPhygTQAZSiBmKxPn4YqesVn9xJz3Jjh+yQEXAm4j9JyrpoZxcAshoAIcPEZICA2ihJ/QyyJpTPrYhbOKFX2dK20eMBgQln",
      "auth_token=ulpMZZxr_GQyRYAylN_Pd-fDCe3UcFl2IOieOqYzq54uXN7sncpM-jvlF-qBFTo7PRt5b6XjxiozWa9_IyAq8SbvqCb7zJ_cKl1tAWtqGaDRh31lKnCU3UDhVg"
    ],
    "source": "default"
  },
  "message_type": 103,
  "msg_elements": [
    {
      "attachments": [
        {
          "content_type": "file",
          "filename": "README.md",
          "size": 1700,
          "url": "https://gzc-download.ftn.qq.com/ftn_handler/6cff3992f36d1762c5cb150dd5d1e6acdf38c0d90cca4abc80bf6f2b9d56bb3f3298bac72e1e4f0a1aadc99c1ef514e0122255d1337b24210e6ce3a2d4d83759?fname=README.md"
        }
      ],
      "content": "",
      "message_type": 103,
      "msg_idx": "REFIDX_SX8TGkckVZgyYb3U3Hoxp73JzqGDQodXzUPhygTQAZSiBmKxPn4YqesVn9xJz3Jjh+yQEXAm4j9JyrpoZxcAshoAIcPEZICA2ihJ/QyyJpTPrYhbOKFX2dK20eMBgQln"
    }
  ],
  "parallel_message": {
    "msg_nodes": [
      {"content": "[文件]", "message_type": 7}
    ]
  },
  "timestamp": "2026-10-29T17:55:24+08:00"
})JSON";

/// 本条的引用索引 (`message_scene.ext` 里的 `msg_idx`)
constexpr const char* kExpectedReplyRef
    = "REFIDX_w8FJB+sgWPQTyqKSZV1/R73JzqGDQodXzUPhygTQAZSiBmKxPn4YqesVn9xJz3Jjh"
      "+yQEXAm4j9JyrpoZxcAshoAIcPEZICA2ihJ/QyyJpTPrYhbOKFX2dK20eMBgQln";

/// 群里普通 @ 消息 (`message_scene.ext` 为**字符串**形态, 与上面的数组形态并存)
constexpr const char* kRealPlainGroupEvent = R"JSON({
  "author": {"bot": false, "id": "U-1", "member_role": "member", "username": "小张"},
  "content": "@机器人 在吗",
  "group_openid": "G-OPENID",
  "id": "ROBOT1.0_plain",
  "message_scene": {"ext": "msg_idx=REFIDX_PLAIN\tref_msg_idx=REFIDX_OTHER"},
  "message_type": 0,
  "timestamp": "2026-10-29T10:00:00+08:00"
})JSON";

bool contains(const std::string& text, std::string_view part) {
    return text.find(part) != std::string::npos;
}

} // namespace

TestResult testChatbotQq() {
    g_cq_passed = 0;
    g_cq_failed = 0;

    // ===================== 1. 真机事件: 引用群文件 =====================
    {
        InboundEvent out;
        const auto   data = Json::parse(kRealQuotedFileEvent);
        XX_TEST_EXPECT_TRUE(normalizeQqEvent("GROUP_AT_MESSAGE_CREATE", data, out));
        XX_TEST_EXPECT_EQ(out.type, ChatType::Group);
        XX_TEST_EXPECT_EQ(out.chatId, std::string{"0D5F5A2F37EFDF78CD830538C4D1B0F8"});
        XX_TEST_EXPECT_EQ(out.senderName, std::string{"寒光"});
        XX_TEST_EXPECT_EQ(out.senderRole, std::string{"owner"});
        XX_TEST_EXPECT_EQ(out.senderId, std::string{"BFD86A0EA982C29C0ACC9F1F0EA38E00"});
        XX_TEST_EXPECT_TRUE(out.mentionedMe);
        XX_TEST_EXPECT_FALSE(out.fromFullMode);
        XX_TEST_EXPECT_TRUE(out.platformMessageId.rfind("ROBOT1.0_", 0) == 0);
        XX_TEST_EXPECT_TRUE(out.timestampMs > 0);

        // 用户打的那句话 (首尾空格原样保留, 由上层裁剪)
        XX_TEST_EXPECT_TRUE(contains(out.text, "看看这个文件的内容是什么"));

        // 被引用的是文件消息: 附件必须被认出来, 否则模型永远"看不到文件"
        XX_TEST_EXPECT_EQ(out.attachments.size(), size_t{1});
        if (out.attachments.size() == 1) {
            const auto& file = out.attachments.front();
            XX_TEST_EXPECT_EQ(file.kind, MediaKind::File);
            XX_TEST_EXPECT_EQ(file.fileName, std::string{"README.md"});
            XX_TEST_EXPECT_EQ(file.sizeBytes, uint64_t{1700});
            XX_TEST_EXPECT_EQ(file.mimeType, std::string{"file"});
            XX_TEST_EXPECT_TRUE(file.url.rfind("https://gzc-download.ftn.qq.com/", 0) == 0);
        }

        // 引用内容没有正文 → 用附件描述顶上, 模型才知道"引用了一条文件消息"
        XX_TEST_EXPECT_EQ(out.quotedText, std::string{"[文件] README.md"});

        // 引用回复句柄取本条的 msg_idx, 不能取成 ref_msg_idx (那是被引用的那条)
        XX_TEST_EXPECT_EQ(out.replyRef, std::string{kExpectedReplyRef});
        XX_TEST_EXPECT_TRUE(out.replyRef != out.platformMessageId);
    }

    // ===================== 2. ext 是字符串形态时也要能取到 =====================
    {
        InboundEvent out;
        const auto   data = Json::parse(kRealPlainGroupEvent);
        XX_TEST_EXPECT_TRUE(normalizeQqEvent("GROUP_AT_MESSAGE_CREATE", data, out));
        XX_TEST_EXPECT_EQ(out.replyRef, std::string{"REFIDX_PLAIN"});
        XX_TEST_EXPECT_EQ(out.chatId, std::string{"G-OPENID"});
        XX_TEST_EXPECT_EQ(out.senderRole, std::string{"member"});
        XX_TEST_EXPECT_TRUE(out.attachments.empty());
        XX_TEST_EXPECT_TRUE(out.quotedText.empty());
        // @ 判定只看事件类型
        XX_TEST_EXPECT_TRUE(out.mentionedMe);
    }

    // ===================== 3. 全量消息事件: 不算 @ =====================
    {
        InboundEvent out;
        const auto   data = Json::parse(kRealPlainGroupEvent);
        XX_TEST_EXPECT_TRUE(normalizeQqEvent("GROUP_MESSAGE_CREATE", data, out));
        XX_TEST_EXPECT_FALSE(out.mentionedMe);
        XX_TEST_EXPECT_TRUE(out.fromFullMode);
    }

    // ===================== 4. 本条消息自己的附件: 三类分类 =====================
    {
        const auto data = Json::parse(R"JSON({
          "author": {"bot": false, "id": "U-2", "username": "小李"},
          "content": "看这几样",
          "group_openid": "G-OPENID",
          "id": "ROBOT1.0_attach",
          "timestamp": "2026-10-29T10:00:00+08:00",
          "attachments": [
            {"content_type": "image/png", "filename": "a.png", "url": "https://x/a.png", "size": 10},
            {"content_type": "video/mp4", "filename": "b.mp4", "url": "https://x/b.mp4", "size": 20},
            {"content_type": "file", "filename": "c.txt", "url": "https://x/c.txt", "size": 30}
          ]
        })JSON");
        InboundEvent out;
        XX_TEST_EXPECT_TRUE(normalizeQqEvent("GROUP_AT_MESSAGE_CREATE", data, out));
        XX_TEST_EXPECT_EQ(out.attachments.size(), size_t{3});
        if (out.attachments.size() == 3) {
            XX_TEST_EXPECT_EQ(out.attachments[0].kind, MediaKind::Image);
            XX_TEST_EXPECT_EQ(out.attachments[1].kind, MediaKind::Video);
            XX_TEST_EXPECT_EQ(out.attachments[2].kind, MediaKind::File);
            XX_TEST_EXPECT_EQ(out.attachments[2].fileName, std::string{"c.txt"});
        }
        // 普通文件不写进正文, 由调度层决定怎么给模型
        XX_TEST_EXPECT_EQ(out.text, std::string{"看这几样"});
    }

    // ===================== 5. 语音: 优先用平台识别文本 =====================
    {
        const auto data = Json::parse(R"JSON({
          "author": {"bot": false, "id": "U-3", "username": "小王"},
          "content": "",
          "group_openid": "G-OPENID",
          "id": "ROBOT1.0_voice",
          "timestamp": "2026-10-29T10:00:00+08:00",
          "attachments": [
            {"content_type": "voice", "url": "https://x/v.amr", "asr_refer_text": "你好呀"}
          ]
        })JSON");
        InboundEvent out;
        XX_TEST_EXPECT_TRUE(normalizeQqEvent("GROUP_AT_MESSAGE_CREATE", data, out));
        XX_TEST_EXPECT_EQ(out.attachments.size(), size_t{1});
        if (out.attachments.size() == 1) {
            XX_TEST_EXPECT_EQ(out.attachments[0].kind, MediaKind::Audio);
        }
        // 识别文本进正文: 不必让模型去听
        XX_TEST_EXPECT_EQ(out.text, std::string{"你好呀"});
    }

    // ===================== 6. 引用一条图片消息: 附件照样给出来 =====================
    {
        const auto data = Json::parse(R"JSON({
          "author": {"bot": false, "id": "U-4", "username": "小赵"},
          "content": "这是谁发的",
          "group_openid": "G-OPENID",
          "id": "ROBOT1.0_quoteimg",
          "message_type": 103,
          "timestamp": "2026-10-29T10:00:00+08:00",
          "msg_elements": [
            {"content": "", "message_type": 103,
             "attachments": [
               {"content_type": "image/jpeg", "url": "https://x/q.jpg", "size": 40}
             ]}
          ]
        })JSON");
        InboundEvent out;
        XX_TEST_EXPECT_TRUE(normalizeQqEvent("GROUP_AT_MESSAGE_CREATE", data, out));
        XX_TEST_EXPECT_EQ(out.attachments.size(), size_t{1});
        if (out.attachments.size() == 1) {
            XX_TEST_EXPECT_EQ(out.attachments[0].kind, MediaKind::Image);
        }
        XX_TEST_EXPECT_EQ(out.quotedText, std::string{"[图片]"});
    }

    // ===================== 7. 引用消息有正文: 保留原文, 不覆盖成附件描述 =====================
    {
        const auto data = Json::parse(R"JSON({
          "author": {"bot": false, "id": "U-5", "username": "小钱"},
          "content": "什么意思",
          "group_openid": "G-OPENID",
          "id": "ROBOT1.0_quotetext",
          "message_type": 103,
          "timestamp": "2026-10-29T10:00:00+08:00",
          "msg_elements": [
            {"author": {"username": "小孙"}, "content": "今天休息", "message_type": 0}
          ]
        })JSON");
        InboundEvent out;
        XX_TEST_EXPECT_TRUE(normalizeQqEvent("GROUP_AT_MESSAGE_CREATE", data, out));
        XX_TEST_EXPECT_EQ(out.quotedText, std::string{"今天休息"});
        XX_TEST_EXPECT_EQ(out.quotedSender, std::string{"小孙"});
        XX_TEST_EXPECT_TRUE(out.attachments.empty());
    }

    // ===================== 8. 私聊: 会话 id 取发送者 =====================
    {
        const auto data = Json::parse(R"JSON({
          "author": {"bot": false, "id": "PRIVATE-1", "username": "小周"},
          "content": "在吗",
          "id": "ROBOT1.0_private",
          "timestamp": "2026-10-29T10:00:00+08:00"
        })JSON");
        InboundEvent out;
        XX_TEST_EXPECT_TRUE(normalizeQqEvent("C2C_MESSAGE_CREATE", data, out));
        XX_TEST_EXPECT_EQ(out.type, ChatType::Private);
        XX_TEST_EXPECT_EQ(out.chatId, std::string{"PRIVATE-1"});
        XX_TEST_EXPECT_TRUE(out.mentionedMe);
    }

    // ===================== 9. 该丢的事件 =====================
    {
        InboundEvent out;
        // 非消息类事件
        XX_TEST_EXPECT_FALSE(
            normalizeQqEvent("GROUP_ADD_ROBOT", Json::parse(R"JSON({"group_openid":"G"})JSON"), out)
        );
        // 机器人自己发的 (平台可能推回来)
        XX_TEST_EXPECT_FALSE(normalizeQqEvent(
            "GROUP_AT_MESSAGE_CREATE",
            Json::parse(R"JSON({
              "author": {"bot": true, "id": "BOT"},
              "content": "我是机器人",
              "group_openid": "G",
              "id": "ROBOT1.0_self",
              "timestamp": "2026-10-29T10:00:00+08:00"
            })JSON"),
            out
        ));
        // 缺会话 id
        XX_TEST_EXPECT_FALSE(normalizeQqEvent(
            "GROUP_AT_MESSAGE_CREATE",
            Json::parse(R"JSON({
              "author": {"bot": false, "id": "U-6"},
              "content": "没群号",
              "id": "ROBOT1.0_nogroup",
              "timestamp": "2026-10-29T10:00:00+08:00"
            })JSON"),
            out
        ));
        // 内容与附件都空 (表情/系统提示): 不叫醒模型
        XX_TEST_EXPECT_FALSE(normalizeQqEvent(
            "GROUP_AT_MESSAGE_CREATE",
            Json::parse(R"JSON({
              "author": {"bot": false, "id": "U-7"},
              "content": "",
              "group_openid": "G",
              "id": "ROBOT1.0_empty",
              "timestamp": "2026-10-29T10:00:00+08:00"
            })JSON"),
            out
        ));
    }

    // ===================== 10. 卡片消息 (message_type=3): 用 ark_data 拼一行 =====================
    {
        const auto data = Json::parse(R"JSON({
          "author": {"bot": false, "id": "U-8", "username": "小吴"},
          "content": "",
          "group_openid": "G-OPENID",
          "id": "ROBOT1.0_ark",
          "message_type": 3,
          "timestamp": "2026-10-29T10:00:00+08:00",
          "ark_data": {
            "prompt": "邀请你加入",
            "fields": [{"title": "群名", "desc": "后端讨论组"}]
          }
        })JSON");
        InboundEvent out;
        XX_TEST_EXPECT_TRUE(normalizeQqEvent("GROUP_AT_MESSAGE_CREATE", data, out));
        XX_TEST_EXPECT_TRUE(contains(out.text, "邀请你加入"));
        XX_TEST_EXPECT_TRUE(contains(out.text, "后端讨论组"));
    }

    // ===================== 11. 文件名兜底: 平台没给 filename 时从 URL 取 =====================
    {
        XX_TEST_EXPECT_EQ(qqFileNameOfUrl("https://x/a/b/c.txt?fname=c.txt"), std::string{"c.txt"});
        XX_TEST_EXPECT_EQ(qqFileNameOfUrl("https://x/a/b/"), std::string{"attachment"});
        XX_TEST_EXPECT_EQ(qqFileNameOfUrl(""), std::string{"attachment"});
    }

    return TestResult{g_cq_passed, g_cq_failed};
}

} // namespace test
} // namespace agentxx
