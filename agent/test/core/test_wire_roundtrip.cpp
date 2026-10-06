/// test_wire_roundtrip —— Wire 协议消息往返与字段保真 (计划 PRO-1 / TST-2 / PRO-7 / PRO-11)
///
/// 覆盖:
/// - 每条消息: `WsAgentIOTransport::serialize` → `deserialize` 后回到同一变体成员,
///   关键字段逐一比对 (含可选字段缺省、bool/整数边界、UTF-8 文本、嵌套结构)
/// - 序列化幂等: 二次序列化得到同一 JSON (字段名/取值稳定, 便于对端缓存与 diff)
/// - 未知字段被忽略 (老客户端读新服务端消息不失败)、未知消息类型被拒绝
/// - WireError 错误码常量 (SessionMismatch 等) 随之往返, 供客户端按码判断
#include "agentxx-test/core/test_wire_roundtrip.h"

#include "agentxx/agent/io/agent_io_transport.h"
#include "agentxx/agent/io/wire_protocol.h"
#include "agentxx/agent/io/ws_io_transport.h"
#include "agentxx/agent/conversation_types.h"
#include <string>
#include <variant>
#include <vector>

namespace {
// 本模块测试计数器 (仅本编译单元可见; 不经头文件 extern 导出)
int g_wr_passed = 0;
int g_wr_failed = 0;
} // namespace

// 断言计数宏覆盖: 将 test_framework.h 的 XX_TEST_EXPECT_* 映射到本模块计数器
#define XX_TEST_PASSED g_wr_passed
#define XX_TEST_FAILED g_wr_failed

namespace agentxx {
namespace test {

using namespace agentxx::agent;
using agentxx::agent::WsAgentIOTransport;

namespace {

/// 往返一次并断言结果 (失败时计入本模块失败数)
/// - 序列化 → 反序列化 → 再序列化, 断言两份 JSON 完全一致 (字段稳定, 便于对端缓存)
/// - 反序列化结果必须回到同一变体成员 (消息类型不漂移)
template<typename T>
T roundTrip(const T& msg) {
    const auto j1   = WsAgentIOTransport::serialize(WireMessage{msg});
    auto       back = WsAgentIOTransport::deserialize(j1);
    if (!back.has_value() || !std::holds_alternative<T>(back.value())) {
        ++g_wr_failed;
        return T{};
    }
    if (WsAgentIOTransport::serialize(back.value()) != j1) {
        ++g_wr_failed;
    } else {
        ++g_wr_passed;
    }
    return std::get<T>(back.value());
}

} // namespace

TestResult testWireRoundtrip() {
    g_wr_passed = 0;
    g_wr_failed = 0;

    // ---------------- 握手 ----------------

    {
        WireHelloAck ack;
        ack.ok        = true;
        ack.sessionId = "sess-中文-1";
        ack.tailHash  = "deadbeef";
        ack.models    = {"gpt-x", "claude-y"};
        ack.deviceId  = "device-42";
        ack.workDir   = "D:/work/项目";
        ack.plugins.push_back(
            WireHelloAck::PluginInfo{
                .name       = "agentxx_filesystem",
                .version    = "1.2.3",
                .interfaces = {"agentxx.agent.tools", "agentxx.agent.permission"},
            }
        );
        auto back = roundTrip(ack);
        XX_TEST_EXPECT_TRUE(back.ok);
        XX_TEST_EXPECT_EQ(back.sessionId, std::string{"sess-中文-1"});
        XX_TEST_EXPECT_EQ(back.tailHash, std::string{"deadbeef"});
        XX_TEST_EXPECT_EQ(back.models.size(), size_t{2});
        XX_TEST_EXPECT_EQ(back.deviceId, std::string{"device-42"});
        XX_TEST_EXPECT_EQ(back.workDir, std::string{"D:/work/项目"});
        XX_TEST_EXPECT_EQ(back.plugins.size(), size_t{1});
        if (!back.plugins.empty()) {
            XX_TEST_EXPECT_EQ(back.plugins[0].name, std::string{"agentxx_filesystem"});
            XX_TEST_EXPECT_EQ(back.plugins[0].version, std::string{"1.2.3"});
            XX_TEST_EXPECT_EQ(back.plugins[0].interfaces.size(), size_t{2});
        }
    }

    // ---------------- 轮次与统计 ----------------

    {
        WireTurnResult r;
        r.sessionId    = "s1";
        r.errorMessage = "取消: 用户中止";
        r.startTimeMs  = 1700000000123LL;
        r.durationMs   = 4567;
        r.hasError     = true;
        r.interrupted  = true;
        auto back      = roundTrip(r);
        XX_TEST_EXPECT_EQ(back.sessionId, std::string{"s1"});
        XX_TEST_EXPECT_EQ(back.errorMessage, std::string{"取消: 用户中止"});
        XX_TEST_EXPECT_EQ(back.startTimeMs, 1700000000123LL);
        XX_TEST_EXPECT_EQ(back.durationMs, 4567);
        XX_TEST_EXPECT_TRUE(back.hasError);
        XX_TEST_EXPECT_TRUE(back.interrupted);
    }
    {
        WireContextStats st;
        st.contextTokens    = 123456;
        st.maxContextTokens = 200000;
        st.tps              = 12.5;
        auto back           = roundTrip(st);
        XX_TEST_EXPECT_EQ(back.contextTokens, uint64_t{123456});
        XX_TEST_EXPECT_EQ(back.maxContextTokens, uint64_t{200000});
        XX_TEST_EXPECT_TRUE(back.tps > 12.49 && back.tps < 12.51);
    }

    // ---------------- 错误对象 (PRO-11: 文本与机器码分开) ----------------

    {
        WireError e;
        e.code    = WireErrorCode::SessionMismatch;
        e.message = "request 'user_input' targets another session";
        auto back = roundTrip(e);
        XX_TEST_EXPECT_EQ(back.code, WireErrorCode::SessionMismatch);
        XX_TEST_EXPECT_EQ(back.message, e.message);
        // 未知码按 Internal 处理的约定: 码值本身照原样传递 (客户端负责兜底)
        XX_TEST_EXPECT_EQ(WireErrorCode::Internal, 0);
    }

    // ---------------- 模型 ----------------

    {
        WireModelInfo info;
        info.currentModel = "gpt-x";
        info.models       = {"gpt-x", "claude-y"};
        info.capabilities.push_back(
            ModelCapabilityInfo{
                .name       = "gpt-x",
                .imageInput = true,
                .audioInput = false,
                .videoInput = false,
            }
        );
        auto back = roundTrip(info);
        XX_TEST_EXPECT_EQ(back.currentModel, std::string{"gpt-x"});
        XX_TEST_EXPECT_EQ(back.models.size(), size_t{2});
        XX_TEST_EXPECT_EQ(back.capabilities.size(), size_t{1});
        if (!back.capabilities.empty()) {
            XX_TEST_EXPECT_EQ(back.capabilities[0].name, std::string{"gpt-x"});
            XX_TEST_EXPECT_TRUE(back.capabilities[0].imageInput);
            XX_TEST_EXPECT_FALSE(back.capabilities[0].audioInput);
            XX_TEST_EXPECT_TRUE(back.capabilities[0].hasMultimodalInput());
        }
    }
    {
        auto back = roundTrip(WireGetModel{.sessionId = "s2"});
        XX_TEST_EXPECT_EQ(back.sessionId, std::string{"s2"});
    }
    {
        auto back = roundTrip(WireSelectModel{.sessionId = "s2", .model = "claude-y"});
        XX_TEST_EXPECT_EQ(back.sessionId, std::string{"s2"});
        XX_TEST_EXPECT_EQ(back.model, std::string{"claude-y"});
    }

    // ---------------- 上下文相关请求/响应 ----------------

    {
        auto back = roundTrip(WireGetContext{.sessionId = "s3"});
        XX_TEST_EXPECT_EQ(back.sessionId, std::string{"s3"});
    }
    {
        auto back = roundTrip(WireCompactContext{.sessionId = "s3"});
        XX_TEST_EXPECT_EQ(back.sessionId, std::string{"s3"});
    }
    {
        WireContextMessages m;
        m.messages = utilxx_base::Json::array({
            utilxx_base::Json{{"role", "system"}, {"content", "sys"}},
            utilxx_base::Json{{"role", "user"}, {"content", "你好"}},
        });
        auto back = roundTrip(m);
        XX_TEST_EXPECT_EQ(back.messages.size(), size_t{2});
        if (back.messages.size() == 2) {
            XX_TEST_EXPECT_EQ(back.messages[1].value("content", std::string{}), std::string{"你好"});
        }
    }

    // ---------------- 历史分页 ----------------

    {
        WireGetViewMessages req;
        req.sessionId   = "s4";
        req.beforeIndex = 512;
        req.count       = 100;
        auto back       = roundTrip(req);
        XX_TEST_EXPECT_EQ(back.sessionId, std::string{"s4"});
        XX_TEST_EXPECT_EQ(back.beforeIndex, uint64_t{512});
        XX_TEST_EXPECT_EQ(back.count, uint32_t{100});
    }
    {
        WireViewMessagesPage page;
        page.sessionId  = "s4";
        page.startIndex = 412;
        page.totalCount = 512;
        page.messages.push_back(ViewMessage::makeText(ViewMessage::Role::User, "hi", 111, 222));
        page.messages.push_back(ViewMessage::makeText(ViewMessage::Role::Assistant, "hello"));
        auto back = roundTrip(page);
        XX_TEST_EXPECT_EQ(back.sessionId, std::string{"s4"});
        XX_TEST_EXPECT_EQ(back.startIndex, uint64_t{412});
        XX_TEST_EXPECT_EQ(back.totalCount, uint64_t{512});
        XX_TEST_EXPECT_EQ(back.messages.size(), size_t{2});
        if (back.messages.size() == 2) {
            XX_TEST_EXPECT_TRUE(back.messages[0].role == ViewMessage::Role::User);
            XX_TEST_EXPECT_EQ(back.messages[0].text, std::string{"hi"});
            XX_TEST_EXPECT_EQ(back.messages[0].startTimeMs, 111);
            XX_TEST_EXPECT_EQ(back.messages[0].durationMs, 222);
            XX_TEST_EXPECT_TRUE(back.messages[1].role == ViewMessage::Role::Assistant);
        }
    }

    // ---------------- 会话列表与切换 ----------------

    {
        WireListSessions req;
        req.beforeMs = 1700000000000LL;
        req.beforeId = "s10";
        req.limit    = 20;
        auto back    = roundTrip(req);
        XX_TEST_EXPECT_EQ(back.beforeMs, 1700000000000LL);
        XX_TEST_EXPECT_EQ(back.beforeId, std::string{"s10"});
        XX_TEST_EXPECT_EQ(back.limit, uint32_t{20});
    }
    {
        WireSessionList list;
        list.totalCount = 3;
        list.hasMore    = true;
        SessionInfo info;
        info.sessionId    = "s10";
        info.title        = "会话标题";
        info.lastActiveMs = 1700000000000LL;
        list.sessions.push_back(std::move(info));
        auto back = roundTrip(list);
        XX_TEST_EXPECT_EQ(back.sessions.size(), size_t{1});
        XX_TEST_EXPECT_EQ(back.totalCount, uint64_t{3});
        XX_TEST_EXPECT_TRUE(back.hasMore);
        if (!back.sessions.empty()) {
            XX_TEST_EXPECT_EQ(back.sessions[0].sessionId, std::string{"s10"});
            XX_TEST_EXPECT_EQ(back.sessions[0].title, std::string{"会话标题"});
            XX_TEST_EXPECT_EQ(back.sessions[0].lastActiveMs, 1700000000000LL);
        }
    }
    {
        auto back = roundTrip(WireSwitchSession{.sessionId = "s10"});
        XX_TEST_EXPECT_EQ(back.sessionId, std::string{"s10"});
    }

    // ---------------- 队列与取消 ----------------

    {
        WireMessageQueueUpdate up;
        up.sessionId = "s5";
        MessageQueueItem item;
        item.id          = "q1";
        item.text        = "排队消息";
        item.model       = "gpt-x";
        item.createdAtMs = 1700000000999LL;
        up.items.push_back(std::move(item));
        auto back = roundTrip(up);
        XX_TEST_EXPECT_EQ(back.sessionId, std::string{"s5"});
        XX_TEST_EXPECT_EQ(back.items.size(), size_t{1});
        if (!back.items.empty()) {
            XX_TEST_EXPECT_EQ(back.items[0].id, std::string{"q1"});
            XX_TEST_EXPECT_EQ(back.items[0].text, std::string{"排队消息"});
            XX_TEST_EXPECT_EQ(back.items[0].createdAtMs, 1700000000999LL);
        }
    }
    {
        auto back = roundTrip(WireClearMessageQueue{.sessionId = "s5"});
        XX_TEST_EXPECT_EQ(back.sessionId, std::string{"s5"});
    }
    {
        auto back
            = roundTrip(WireRemoveQueueItem{.sessionId = "s5", .itemId = "q1"});
        XX_TEST_EXPECT_EQ(back.sessionId, std::string{"s5"});
        XX_TEST_EXPECT_EQ(back.itemId, std::string{"q1"});
    }
    {
        auto back = roundTrip(WireInterruptAndRunNext{.sessionId = "s5"});
        XX_TEST_EXPECT_EQ(back.sessionId, std::string{"s5"});
    }
    {
        auto back = roundTrip(WireCancel{.sessionId = "s5"});
        XX_TEST_EXPECT_EQ(back.sessionId, std::string{"s5"});
    }

    // ---------------- 中断 ----------------

    {
        WireInterruptRequest req;
        req.id        = 42;
        req.sessionId = "s6";
        req.node      = "xx_Toolcall";
        req.value     = "repeat_toolcall";
        req.argJson   = R"({"tool_name":"x"})";
        auto back     = roundTrip(req);
        XX_TEST_EXPECT_EQ(back.id, int64_t{42});
        XX_TEST_EXPECT_EQ(back.sessionId, std::string{"s6"});
        XX_TEST_EXPECT_EQ(back.node, std::string{"xx_Toolcall"});
        XX_TEST_EXPECT_EQ(back.value, std::string{"repeat_toolcall"});
        XX_TEST_EXPECT_EQ(back.argJson, std::string{R"({"tool_name":"x"})"});
    }
    {
        WireInterruptResponse resp;
        resp.id     = 42;
        resp.result = utilxx_base::Json{{"allow", "true"}};
        auto back   = roundTrip(resp);
        XX_TEST_EXPECT_EQ(back.id, int64_t{42});
        XX_TEST_EXPECT_EQ(back.result.value("allow", std::string{}), std::string{"true"});
    }
    {
        auto back = roundTrip(WireInterruptExpired{.id = 42, .sessionId = "s6"});
        XX_TEST_EXPECT_EQ(back.id, int64_t{42});
        XX_TEST_EXPECT_EQ(back.sessionId, std::string{"s6"});
    }

    // ---------------- 插件数据 ----------------

    {
        WirePluginData p;
        p.plugin = "agentxx_codegraph";
        p.event  = "status";
        p.data   = R"({"ok":true})";
        auto back = roundTrip(p);
        XX_TEST_EXPECT_EQ(back.plugin, std::string{"agentxx_codegraph"});
        XX_TEST_EXPECT_EQ(back.event, std::string{"status"});
        XX_TEST_EXPECT_EQ(back.data, std::string{R"({"ok":true})"});
    }
    {
        WirePluginDataUp p;
        p.plugin = "agentxx_host";
        p.event  = "client_interfaces";
        p.data   = R"({"interfaces":["ui"]})";
        auto back = roundTrip(p);
        XX_TEST_EXPECT_EQ(back.plugin, std::string{"agentxx_host"});
        XX_TEST_EXPECT_EQ(back.event, std::string{"client_interfaces"});
        XX_TEST_EXPECT_EQ(back.data, std::string{R"({"interfaces":["ui"]})"});
    }

    // ---------------- 组件信息 ----------------

    {
        auto back = roundTrip(WireGetAppendComponentInfo{.sessionId = "s7"});
        XX_TEST_EXPECT_EQ(back.sessionId, std::string{"s7"});
    }
    {
        WireAppendComponentInfo info;
        AppendComponentNotification n;
        n.type         = AppendComponentNotification::Type::Plugin;
        n.name         = "agentxx_filesystem";
        n.success      = true;
        n.errorMessage = "";
        info.notifications.push_back(std::move(n));
        AppendComponentNotification failed;
        failed.type         = AppendComponentNotification::Type::Mcp;
        failed.name         = "broken-mcp";
        failed.success      = false;
        failed.errorMessage = "connect failed";
        info.notifications.push_back(std::move(failed));
        auto back = roundTrip(info);
        XX_TEST_EXPECT_EQ(back.notifications.size(), size_t{2});
        if (back.notifications.size() == 2) {
            XX_TEST_EXPECT_TRUE(
                back.notifications[0].type == AppendComponentNotification::Type::Plugin
            );
            XX_TEST_EXPECT_EQ(back.notifications[0].name, std::string{"agentxx_filesystem"});
            XX_TEST_EXPECT_TRUE(back.notifications[0].success);
            XX_TEST_EXPECT_FALSE(back.notifications[1].success);
            XX_TEST_EXPECT_EQ(back.notifications[1].errorMessage, std::string{"connect failed"});
        }
    }

    // ---------------- 权限状态 ----------------

    {
        roundTrip(WireGetPermissionState{});
        auto on = roundTrip(WireSetFullAuth{.fullAuth = true});
        XX_TEST_EXPECT_TRUE(on.fullAuth);
        auto off = roundTrip(WireSetFullAuth{.fullAuth = false});
        XX_TEST_EXPECT_FALSE(off.fullAuth);
        auto st = roundTrip(WirePermissionState{.fullAuth = true});
        XX_TEST_EXPECT_TRUE(st.fullAuth);
    }

    // ---------------- 目录列举 ----------------

    {
        WireListDir req;
        req.reqId             = 7;
        req.path              = "D:/素材";
        req.allowedExtensions = {"png", "jpg"};
        auto back             = roundTrip(req);
        XX_TEST_EXPECT_EQ(back.reqId, uint64_t{7});
        XX_TEST_EXPECT_EQ(back.path, std::string{"D:/素材"});
        XX_TEST_EXPECT_EQ(back.allowedExtensions.size(), size_t{2});
    }
    {
        WireListDirResult r;
        r.reqId      = 7;
        r.ok         = true;
        r.currentDir = "D:/素材";
        r.parentDir  = "D:/";
        WireDirEntry e;
        e.name      = "图.png";
        e.fullPath  = "D:/素材/图.png";
        e.sizeBytes = 1024;
        e.isDir     = false;
        e.supported = true;
        e.mediaType = MediaType::Image;
        r.entries.push_back(std::move(e));
        auto back = roundTrip(r);
        XX_TEST_EXPECT_EQ(back.reqId, uint64_t{7});
        XX_TEST_EXPECT_TRUE(back.ok);
        XX_TEST_EXPECT_EQ(back.currentDir, std::string{"D:/素材"});
        XX_TEST_EXPECT_EQ(back.parentDir, std::string{"D:/"});
        XX_TEST_EXPECT_EQ(back.entries.size(), size_t{1});
        if (!back.entries.empty()) {
            XX_TEST_EXPECT_EQ(back.entries[0].name, std::string{"图.png"});
            XX_TEST_EXPECT_EQ(back.entries[0].sizeBytes, uint64_t{1024});
            XX_TEST_EXPECT_TRUE(back.entries[0].mediaType == MediaType::Image);
        }
    }

    // ---------------- 新增模型配置 ----------------

    {
        WireAddModel m;
        m.sessionId                 = "s8";
        m.name                      = "本机 ollama";
        m.modelType                 = "openai";
        m.baseUrl                   = "http://127.0.0.1:11434/v1";
        m.apiPath                   = "/chat/completions";
        m.apiKey                    = "EMPTY";
        m.modelName                 = "qwen3";
        m.modelContextMaxToken      = 131072;
        m.maxConcurrentConnections  = 3;
        m.connectTimeoutSeconds     = 8;
        m.readChunkTimeoutSeconds   = 30;
        m.sslVerify                 = 0;
        m.sendThinking              = true;
        m.requestReasoningSummary   = false;
        m.imageInput                = true;
        m.audioInput                = false;
        m.videoInput                = false;
        m.extraApiConfig            = utilxx_base::Json{{"k", 1}};
        m.extraHeaders              = utilxx_base::Json{{"H", "v"}};
        auto back                   = roundTrip(m);
        XX_TEST_EXPECT_EQ(back.sessionId, std::string{"s8"});
        XX_TEST_EXPECT_EQ(back.name, std::string{"本机 ollama"});
        XX_TEST_EXPECT_EQ(back.baseUrl, std::string{"http://127.0.0.1:11434/v1"});
        XX_TEST_EXPECT_EQ(back.modelContextMaxToken, uint64_t{131072});
        XX_TEST_EXPECT_EQ(back.maxConcurrentConnections, uint64_t{3});
        XX_TEST_EXPECT_EQ(back.connectTimeoutSeconds, int32_t{8});
        XX_TEST_EXPECT_EQ(back.sslVerify, int8_t{0});
        XX_TEST_EXPECT_TRUE(back.sendThinking);
        XX_TEST_EXPECT_FALSE(back.requestReasoningSummary);
        XX_TEST_EXPECT_TRUE(back.imageInput);
        XX_TEST_EXPECT_EQ(back.extraApiConfig.value("k", 0), 1);
        XX_TEST_EXPECT_EQ(back.extraHeaders.value("H", std::string{}), std::string{"v"});
    }
    {
        WireAddModelResult r;
        r.ok    = false;
        r.name  = "bad";
        r.error = "name duplicated";
        auto back = roundTrip(r);
        XX_TEST_EXPECT_FALSE(back.ok);
        XX_TEST_EXPECT_EQ(back.name, std::string{"bad"});
        XX_TEST_EXPECT_EQ(back.error, std::string{"name duplicated"});
    }

    // ---------------- 心跳 (裸 JSON: 不进入 WireMessage 变体) ----------------

    {
        const auto ping = io::makePing(1700000000777LL);
        XX_TEST_EXPECT_EQ(ping.value("type", std::string{}), std::string{io::MsgType::Ping});
        XX_TEST_EXPECT_EQ(ping.value("t", int64_t{0}), 1700000000777LL);
        const auto pong = io::makePong(1700000000777LL);
        XX_TEST_EXPECT_EQ(pong.value("type", std::string{}), std::string{io::MsgType::Pong});
        XX_TEST_EXPECT_EQ(pong.value("t", int64_t{0}), 1700000000777LL);
    }

    // ---------------- 日志 ----------------

    {
        auto back = roundTrip(WireLog{.level = 3, .message = "警告: 磁盘写入失败"});
        XX_TEST_EXPECT_EQ(back.level, 3);
        XX_TEST_EXPECT_EQ(back.message, std::string{"警告: 磁盘写入失败"});
    }

    // ---------------- 向前兼容 ----------------

    {
        // 未知字段: 老客户端读新服务端消息时必须忽略而不是失败
        const auto j = utilxx_base::Json{
            {"type",      io::MsgType::TurnResult},
            {"sessionId", "s9"                   },
            {"durationMs", 100                   },
            {"futureField", utilxx_base::Json{{"nested", true}}},
        };
        auto back = WsAgentIOTransport::deserialize(j.dump());
        XX_TEST_EXPECT_TRUE(back.has_value());
        if (back.has_value() && std::holds_alternative<WireTurnResult>(back.value())) {
            const auto& r = std::get<WireTurnResult>(back.value());
            XX_TEST_EXPECT_EQ(r.sessionId, std::string{"s9"});
            XX_TEST_EXPECT_EQ(r.durationMs, 100);
        } else {
            XX_TEST_EXPECT_TRUE(false);
        }

        // 未知消息类型: 明确拒绝 (返回 nullopt), 不抛异常
        const auto unknown = utilxx_base::Json{
            {"type", "future_message_type"},
            {"x",    1                    },
        };
        XX_TEST_EXPECT_FALSE(WsAgentIOTransport::deserialize(unknown.dump()).has_value());
    }

    return TestResult{g_wr_passed, g_wr_failed};
}

} // namespace test
} // namespace agentxx
