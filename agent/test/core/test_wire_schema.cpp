/// test_wire_schema —— Wire 协议字段清单生成与新鲜度校验 (计划 PRO-4 / TST-2)
///
/// 背景: 线协议是手写编解码 (`wire_protocol.cpp`), 字段清单散落在结构体定义与
/// 编解码函数里, 其他语言实现 (或离线工具) 没有可读的单一来源。
///
/// 做法: 每个消息类型准备**一个示例实例** (与 `WireMessage` 变体逐个对应),
/// 序列化成 JSON 后按"字段名 → JSON 类型"生成清单:
/// - `agent/schema/wire-schema.json` (机器可读: 类型标签 + 字段类型 + 示例值)
/// - `docs/zh-cn/design/wire-protocol-fields.md` (人工阅读的字段表)
///
/// 检查: 生成结果与仓库内提交的生成物逐字节比对, 不一致即失败 (提示如何更新)。
/// 变体数量与示例数量必须一致 —— 新增消息类型却忘了补示例时, 本模块立即失败,
/// 避免字段清单悄悄漏掉新消息。
#include "agentxx-test/core/test_wire_schema.h"

#include "agentxx-test/core/schema_artifact.h"
#include "agentxx/agent/io/wire_protocol.h"
#include "utilxx_base/string_util.h"
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fmt/format.h>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace {
// 本模块测试计数器 (仅本编译单元可见; 不经头文件 extern 导出)
int g_ws_passed = 0;
int g_ws_failed = 0;
} // namespace

// 断言计数宏覆盖: 将 test_framework.h 的 XX_TEST_EXPECT_* 映射到本模块计数器
#define XX_TEST_PASSED g_ws_passed
#define XX_TEST_FAILED g_ws_failed

namespace agentxx {
namespace test {

namespace fs = std::filesystem;

using agentxx::agent::io::serialize;
using agentxx::agent::WireProtocol;
using agentxx::agent::AppendComponentNotification;
using agentxx::agent::MediaType;
using agentxx::agent::MessageQueueItem;
using agentxx::agent::ModelCapabilityInfo;
using agentxx::agent::SessionInfo;
using agentxx::agent::ViewMessage;
using agentxx::agent::WireAddModel;
using agentxx::agent::WireAddModelResult;
using agentxx::agent::WireAppendComponentInfo;
using agentxx::agent::WireCancel;
using agentxx::agent::WireClearMessageQueue;
using agentxx::agent::WireCompactContext;
using agentxx::agent::WireContextMessages;
using agentxx::agent::WireContextStats;
using agentxx::agent::WireDelta;
using agentxx::agent::WireDirEntry;
using agentxx::agent::WireError;
using agentxx::agent::WireErrorCode;
using agentxx::agent::WireGetAppendComponentInfo;
using agentxx::agent::WireGetContext;
using agentxx::agent::WireGetModel;
using agentxx::agent::WireGetPermissionState;
using agentxx::agent::WireGetViewMessages;
using agentxx::agent::WireHello;
using agentxx::agent::WireHelloAck;
using agentxx::agent::WireInputAck;
using agentxx::agent::WireInterruptAndRunNext;
using agentxx::agent::WireInterruptExpired;
using agentxx::agent::WireInterruptRequest;
using agentxx::agent::WireInterruptResponse;
using agentxx::agent::WireListDir;
using agentxx::agent::WireListDirResult;
using agentxx::agent::WireListSessions;
using agentxx::agent::WireLog;
using agentxx::agent::WireMessage;
using agentxx::agent::WireMessageQueueUpdate;
using agentxx::agent::WireModelInfo;
using agentxx::agent::WirePermissionState;
using agentxx::agent::WirePluginData;
using agentxx::agent::WirePluginDataUp;
using agentxx::agent::WireRemoveQueueItem;
using agentxx::agent::WireRenameSession;
using agentxx::agent::WireRenameSessionResult;
using agentxx::agent::WireSelectModel;
using agentxx::agent::WireSessionList;
using agentxx::agent::WireSetFullAuth;
using agentxx::agent::WireSwitchSession;
using agentxx::agent::WireSyncPayload;
using agentxx::agent::WireTurnResult;
using agentxx::agent::WireUserInput;
using agentxx::agent::WireViewMessagesPage;
// 模型删除与 host tool (追加在变体末尾)
using agentxx::agent::WireHostToolCall;
using agentxx::agent::WireHostToolInfo;
using agentxx::agent::WireHostToolRegister;
using agentxx::agent::WireHostToolResult;
using agentxx::agent::WireHostToolUnregister;
using agentxx::agent::WireRemoveModel;
using agentxx::agent::WireRemoveModelResult;

namespace {

/// 一条线消息的示例实例 (名称用于阅读, `index` 必须与其在 `WireMessage` 变体中的下标一致)
struct WireSample {
    std::string name;
    WireMessage message;
};

/// JSON 值的类型描述 (供其他语言实现参考; 只展开两层, 更深的嵌套收敛为 `object`)
std::string jsonTypeOf(const utilxx_base::Json& value, int depth = 0) {
    if (value.is_null()) {
        return "null";
    }
    if (value.is_boolean()) {
        return "boolean";
    }
    if (value.is_string()) {
        return "string";
    }
    if (value.is_number_float()) {
        return "number";
    }
    if (value.is_number()) {
        return "integer";
    }
    if (value.is_array()) {
        if (value.empty()) {
            return "array<?>";
        }
        return fmt::format("array<{}>", jsonTypeOf(value.front(), depth + 1));
    }
    if (value.is_object()) {
        if (depth >= 2) {
            return "object";
        }
        std::string out = "object{";
        bool        first = true;
        for (const auto& [key, child] : value.items()) {
            out += fmt::format("{}{}:{}", first ? "" : ", ", key, jsonTypeOf(child, depth + 1));
            first = false;
        }
        out += "}";
        return out;
    }
    return "unknown";
}

/// 全部消息类型的示例实例 (顺序必须与 `WireMessage` 变体一致)
std::vector<WireSample> wireSamples() {
    std::vector<WireSample> out;
    out.reserve(std::variant_size_v<WireMessage>);

    // 1) 握手: 客户端 -> 服务端
    {
        WireHello m;
        m.sessionId       = "sess-中文-1";
        m.token           = "token-abc";
        m.lastSeq         = 42;
        m.tailHash        = "deadbeef";
        m.language        = "zh-cn";
        m.afterViewSeq    = 128;
        m.protocolVersion = WireProtocol::kVersion;
        m.capabilities    = {
            std::string{WireProtocol::kCapInputDelivery},
            std::string{WireProtocol::kCapUiForm},
        };
        out.push_back({"hello", WireMessage{std::move(m)}});
    }
    // 2) 握手: 服务端 -> 客户端
    {
        WireHelloAck m;
        m.ok        = true;
        m.sessionId = "sess-中文-1";
        m.tailHash  = "deadbeef";
        m.models    = {"gpt-x", "claude-y"};
        m.deviceId  = "device-42";
        m.workDir   = "D:/work/项目";
        m.protocolVersion = WireProtocol::kVersion;
        m.capabilities    = agentxx::agent::serverWireCapabilities();
        m.plugins.push_back(
            WireHelloAck::PluginInfo{
                .name       = "agentxx_filesystem",
                .version    = "1.2.3",
                .interfaces = {"agentxx.agent.tools"},
            }
        );
        out.push_back({"hello_ack", WireMessage{std::move(m)}});
    }
    // 3) 用户输入
    {
        WireUserInput m;
        m.sessionId = "s1";
        m.text      = "你好";
        m.model     = "gpt-x";
        m.delivery  = std::string{agentxx::agent::InputDelivery::NextStep};
        m.requestId = 77;
        out.push_back({"user_input", WireMessage{std::move(m)}});
    }
    // 4) 输入受理回执
    {
        WireInputAck m;
        m.requestId = 77;
        m.sessionId = "s1";
        m.delivery  = std::string{agentxx::agent::InputDelivery::NextTurn};
        m.status    = std::string{agentxx::agent::InputStatus::Queued};
        m.reason    = std::string{agentxx::agent::InputRejectReason::EmptyContent};
        m.detail    = "输入为空";
        m.itemId    = "i-9";
        out.push_back({"input_ack", WireMessage{std::move(m)}});
    }
    // 5) 取消
    out.push_back({"cancel", WireMessage{WireCancel{.sessionId = "s1"}}});
    // 6) 切换模型
    out.push_back(
        {"select_model", WireMessage{WireSelectModel{.sessionId = "s1", .model = "claude-y"}}}
    );
    // 7) 中断请求 (服务端 -> 客户端)
    {
        WireInterruptRequest m;
        m.id        = 42;
        m.sessionId = "s1";
        m.node      = "xx_Toolcall";
        m.value     = "repeat_toolcall";
        m.argJson   = R"({"tool_name":"x"})";
        out.push_back({"interrupt_request", WireMessage{std::move(m)}});
    }
    // 8) 中断应答
    {
        WireInterruptResponse m;
        m.id     = 42;
        m.result = utilxx_base::Json{{"allow", "true"}};
        out.push_back({"interrupt_response", WireMessage{std::move(m)}});
    }
    // 9) 中断过期
    out.push_back(
        {"interrupt_expired", WireMessage{WireInterruptExpired{.id = 42, .sessionId = "s1"}}}
    );
    // 10) 会话增量 (流式 token / 消息插入与更新)
    {
        WireDelta m;
        m.type         = WireDelta::Type::InsertMessage;
        m.seq          = 128;
        m.historyCount = 12;
        m.msgId        = "m-12";
        m.text         = "增量文本";
        m.toolName     = "agentxx_filesystem_read";
        m.toolCallId   = "call_1";
        m.arguments    = R"({"path":"a.txt"})";
        m.result       = "文件内容";
        m.nodeName     = "llm";
        m.tailHash     = "cafebabe";
        m.startTimeMs  = 1700000000123LL;
        m.durationMs   = 456;
        m.tps          = 12.5;
        m.tipType      = WireDelta::TipType::Warning;
        m.hasError     = false;
        m.message      = std::make_shared<ViewMessage>(
            ViewMessage::makeText(ViewMessage::Role::Assistant, "插入的完整消息")
        );
        out.push_back({"delta", WireMessage{std::move(m)}});
    }
    // 11) 会话全量/增量同步快照
    {
        WireSyncPayload m;
        m.fromIndex     = 0;
        m.messages      = {ViewMessage::makeText(ViewMessage::Role::User, "历史消息")};
        m.tailHash      = "deadbeef";
        m.totalMessages = 1;
        m.messageQueue.push_back(
            MessageQueueItem{
                .id          = "q-1",
                .text        = "排队消息",
                .model       = "gpt-x",
                .createdAtMs = 1700000000999LL,
                .delivery    = std::string{agentxx::agent::InputDelivery::NextTurn},
                .recovered   = true,
            }
        );
        m.deltaSeq    = 7;
        m.queueState  = std::string{
            agentxx::agent::sessionQueueStateText(agentxx::agent::SessionQueueState::Paused)
        };
        m.lastViewSeq = 5;
        m.incremental = true;
        out.push_back({"sync", WireMessage{std::move(m)}});
    }
    // 12) 轮次结果
    {
        WireTurnResult m;
        m.sessionId    = "s1";
        m.errorMessage = "取消: 用户中止";
        m.startTimeMs  = 1700000000123LL;
        m.durationMs   = 4567;
        m.hasError     = true;
        m.interrupted  = false;
        out.push_back({"turn_result", WireMessage{std::move(m)}});
    }
    // 13) 上下文统计
    {
        WireContextStats m;
        m.contextTokens    = 123456;
        m.maxContextTokens = 200000;
        m.tps              = 12.5;
        out.push_back({"context_stats", WireMessage{std::move(m)}});
    }
    // 14) 错误对象
    {
        WireError m;
        m.code    = WireErrorCode::SessionMismatch;
        m.message = "request targets another session";
        out.push_back({"error", WireMessage{std::move(m)}});
    }
    // 15) 日志
    out.push_back({"log", WireMessage{WireLog{.level = 3, .message = "警告: 写入失败"}}});
    // 16) 请求模型列表
    out.push_back({"get_model", WireMessage{WireGetModel{.sessionId = "s1"}}});
    // 17) 模型信息
    {
        WireModelInfo m;
        m.currentModel = "gpt-x";
        m.models       = {"gpt-x", "claude-y"};
        m.capabilities.push_back(
            ModelCapabilityInfo{
                .name       = "gpt-x",
                .imageInput = true,
                .audioInput = false,
                .videoInput = false,
            }
        );
        out.push_back({"model_info", WireMessage{std::move(m)}});
    }
    // 18) 请求组件加载信息
    out.push_back(
        {"get_append_component_info", WireMessage{WireGetAppendComponentInfo{.sessionId = "s1"}}}
    );
    // 19) 组件加载信息
    {
        WireAppendComponentInfo m;
        m.notifications.push_back(
            AppendComponentNotification{
                .name         = "agentxx_filesystem",
                .errorMessage = "",
                .type         = AppendComponentNotification::Type::Plugin,
                .success      = true,
            }
        );
        m.notifications.push_back(
            AppendComponentNotification{
                .name         = "broken-mcp",
                .errorMessage = "connect failed",
                .type         = AppendComponentNotification::Type::Mcp,
                .success      = false,
            }
        );
        out.push_back({"append_component_info", WireMessage{std::move(m)}});
    }
    // 20) 请求上下文
    out.push_back({"get_context", WireMessage{WireGetContext{.sessionId = "s1"}}});
    // 21) 请求压缩
    out.push_back({"compact_context", WireMessage{WireCompactContext{.sessionId = "s1"}}});
    // 22) 上下文消息
    {
        WireContextMessages m;
        m.messages = utilxx_base::Json::array({
            utilxx_base::Json{{"role", "system"}, {"content", "sys"}},
            utilxx_base::Json{{"role", "user"}, {"content", "你好"}},
        });
        out.push_back({"context_messages", WireMessage{std::move(m)}});
    }
    // 23) 请求会话列表
    {
        WireListSessions m;
        m.beforeMs = 1700000000000LL;
        m.beforeId = "s10";
        m.limit    = 20;
        out.push_back({"list_sessions", WireMessage{std::move(m)}});
    }
    // 24) 会话列表
    {
        WireSessionList m;
        m.totalCount = 3;
        m.hasMore    = true;
        m.sessions.push_back(
            SessionInfo{
                .sessionId    = "s10",
                .title        = "会话标题",
                .lastActiveMs = 1700000000000LL,
            }
        );
        out.push_back({"session_list", WireMessage{std::move(m)}});
    }
    // 25) 切换会话
    out.push_back({"switch_session", WireMessage{WireSwitchSession{.sessionId = "s10"}}});
    // 26) 插件数据 (服务端 -> 客户端)
    {
        WirePluginData m;
        m.plugin = "agentxx_codegraph";
        m.event  = "status";
        m.data   = R"({"ok":true})";
        out.push_back({"plugin_data", WireMessage{std::move(m)}});
    }
    // 27) 插件数据 (客户端 -> 服务端)
    {
        WirePluginDataUp m;
        m.plugin = "agentxx_host";
        m.event  = "client_interfaces";
        m.data   = R"({"interfaces":["ui"]})";
        out.push_back({"plugin_data_up", WireMessage{std::move(m)}});
    }
    // 28) 消息队列更新
    {
        WireMessageQueueUpdate m;
        m.sessionId = "s1";
        m.state     = std::string{
            agentxx::agent::sessionQueueStateText(agentxx::agent::SessionQueueState::Running)
        };
        m.items.push_back(
            MessageQueueItem{
                .id          = "q-1",
                .text        = "排队消息",
                .model       = "",
                .createdAtMs = 1700000000999LL,
                .delivery    = "",
                .recovered   = false,
            }
        );
        out.push_back({"message_queue_update", WireMessage{std::move(m)}});
    }
    // 29) 清空队列
    out.push_back({"clear_message_queue", WireMessage{WireClearMessageQueue{.sessionId = "s1"}}});
    // 30) 删除队列条目
    out.push_back(
        {"remove_queue_item", WireMessage{WireRemoveQueueItem{.sessionId = "s1", .itemId = "q-1"}}}
    );
    // 31) 打断并执行队首
    out.push_back(
        {"interrupt_and_run_next", WireMessage{WireInterruptAndRunNext{.sessionId = "s1"}}}
    );
    // 32) 请求历史分页
    {
        WireGetViewMessages m;
        m.sessionId   = "s1";
        m.beforeIndex = 512;
        m.count       = 100;
        out.push_back({"get_view_messages", WireMessage{std::move(m)}});
    }
    // 33) 历史分页结果
    {
        WireViewMessagesPage m;
        m.sessionId  = "s1";
        m.startIndex = 412;
        m.totalCount = 512;
        m.messages.push_back(ViewMessage::makeText(ViewMessage::Role::User, "hi", 111, 222));
        m.messages.push_back(ViewMessage::makeText(ViewMessage::Role::Assistant, "hello"));
        out.push_back({"view_messages_page", WireMessage{std::move(m)}});
    }
    // 34) 目录列举请求
    {
        WireListDir m;
        m.reqId             = 7;
        m.path              = "D:/素材";
        m.allowedExtensions = {"png", "jpg"};
        out.push_back({"list_dir", WireMessage{std::move(m)}});
    }
    // 35) 目录列举结果
    {
        WireListDirResult m;
        m.reqId      = 7;
        m.ok         = true;
        m.currentDir = "D:/素材";
        m.parentDir  = "D:/";
        m.error      = "";
        m.entries.push_back(
            WireDirEntry{
                .name      = "图.png",
                .fullPath  = "D:/素材/图.png",
                .sizeBytes = 1024,
                .isDir     = false,
                .supported = true,
                .mediaType = MediaType::Image,
            }
        );
        out.push_back({"list_dir_result", WireMessage{std::move(m)}});
    }
    // 36) 请求权限状态
    out.push_back({"get_permission_state", WireMessage{WireGetPermissionState{}}});
    // 37) 设置完全授权
    out.push_back({"set_full_auth", WireMessage{WireSetFullAuth{.fullAuth = true}}});
    // 38) 权限状态
    out.push_back({"permission_state", WireMessage{WirePermissionState{.fullAuth = true}}});
    // 39) 新增模型配置
    {
        WireAddModel m;
        m.sessionId                = "s1";
        m.name                     = "本机 ollama";
        m.modelType                = "openai";
        m.baseUrl                  = "http://127.0.0.1:11434/v1";
        m.apiPath                  = "/chat/completions";
        m.apiKey                   = "EMPTY";
        m.modelName                = "qwen3";
        m.modelContextMaxToken     = 131072;
        m.maxConcurrentConnections = 3;
        m.connectTimeoutSeconds    = 8;
        m.readChunkTimeoutSeconds  = 30;
        m.sslVerify                = 0;
        m.sendThinking             = true;
        m.requestReasoningSummary  = false;
        m.imageInput               = true;
        m.audioInput               = false;
        m.videoInput               = false;
        m.extraApiConfig           = utilxx_base::Json{{"k", 1}};
        m.extraHeaders             = utilxx_base::Json{{"H", "v"}};
        out.push_back({"add_model", WireMessage{std::move(m)}});
    }
    // 40) 新增模型配置结果
    {
        WireAddModelResult m;
        m.ok    = false;
        m.name  = "bad";
        m.error = "name duplicated";
        out.push_back({"add_model_result", WireMessage{std::move(m)}});
    }
    // 41) 会话重命名 (计划 RET-1a)
    {
        WireRenameSession m;
        m.sessionId = "s1";
        m.title     = "重构会话标题";
        out.push_back({"rename_session", WireMessage{std::move(m)}});
    }
    // 42) 会话重命名结果
    {
        WireRenameSessionResult m;
        m.sessionId = "s1";
        m.title     = "重构会话标题";
        m.error     = "";
        m.ok        = true;
        out.push_back({"rename_session_result", WireMessage{std::move(m)}});
    }
    // 43) 删除模型配置 (计划 A1; 追加在变体末尾)
    {
        WireRemoveModel m;
        m.sessionId = "s1";
        m.name      = "gpt-x";
        out.push_back({"remove_model", WireMessage{std::move(m)}});
    }
    // 44) 删除模型配置结果
    {
        WireRemoveModelResult m;
        m.name  = "gpt-x";
        m.ok    = true;
        m.error = "";
        out.push_back({"remove_model_result", WireMessage{std::move(m)}});
    }
    // 45) 客户端注册宿主工具 (计划 A4)
    {
        WireHostToolRegister m;
        m.sessionId = "s1";
        m.tools.push_back(
            WireHostToolInfo{
                .name          = "lumen_open_file",
                .description   = "打开宿主编辑器里的文件",
                .inputSchema   = utilxx_base::Json{
                    {"type",       "object"                                  },
                    {"properties", utilxx_base::Json{{"path", utilxx_base::Json{{"type", "string"}}}}},
                },
                .timeoutSec    = 30,
                .maxConcurrent = 2,
            }
        );
        out.push_back({"host_tool_register", WireMessage{std::move(m)}});
    }
    // 46) 客户端注销宿主工具
    {
        WireHostToolUnregister m;
        m.sessionId = "s1";
        m.names     = {"lumen_open_file"};
        out.push_back({"host_tool_unregister", WireMessage{std::move(m)}});
    }
    // 47) 服务端请求客户端执行宿主工具
    {
        WireHostToolCall m;
        m.callId     = 7;
        m.sessionId  = "s1";
        m.name       = "lumen_open_file";
        m.argsJson   = R"({"path":"/tmp/a.txt"})";
        m.timeoutSec = 30;
        out.push_back({"host_tool_call", WireMessage{std::move(m)}});
    }
    // 48) 客户端返回宿主工具执行结果
    {
        WireHostToolResult m;
        m.callId       = 7;
        m.ok           = true;
        m.resultJson   = R"({"opened":true})";
        m.errorMessage = "";
        out.push_back({"host_tool_result", WireMessage{std::move(m)}});
    }
    return out;
}

/// 生成 schema JSON 文本 (确定性: 字段按序列化顺序列出)
std::string buildSchemaJson(const std::vector<WireSample>& samples) {
    auto messages = utilxx_base::Json::array();
    for (size_t i = 0; i < samples.size(); ++i) {
        const auto& sample = samples[i];
        const auto  json   = utilxx_base::Json::parse(serialize(sample.message));

        auto fields = utilxx_base::Json::object();
        for (const auto& [key, value] : json.items()) {
            fields[key] = jsonTypeOf(value);
        }
        messages.push_back(
            utilxx_base::Json{
                {"index",  static_cast<int64_t>(i)},
                {"name",   sample.name            },
                {"type",   json.value("type", std::string{})},
                {"fields", std::move(fields)      },
                {"sample", json                    },
            }
        );
    }

    utilxx_base::Json root;
    root["protocolVersion"] = WireProtocol::kVersion;
    root["messageCount"]    = static_cast<int64_t>(samples.size());
    root["note"] = "生成物, 请勿手工编辑; 重新生成: AGENTXX_UPDATE_WIRE_SCHEMA=1 agentxx_test "
                   "wire_schema";
    root["messages"] = std::move(messages);
    return root.dump(2) + "\n";
}

/// 生成人工阅读的字段文档
std::string buildSchemaDoc(const std::vector<WireSample>& samples) {
    std::string out;
    out += "# Wire 协议字段清单\n\n";
    out += "> 本文是**生成物**, 请勿手工编辑。字段与示例值的完整形态见\n";
    out += "> `agent/schema/wire-schema.json`。\n>\n";
    out += "> 重新生成: 设 `AGENTXX_UPDATE_WIRE_SCHEMA=1` 运行测试模块 `wire_schema`\n";
    out += "> (生成后请人工 review diff 再提交)。\n>\n";
    out += "> 相关文档: [index.md](index.md) · [配置与设置边界](configuration.md)\n\n";
    out += fmt::format(
        "协议版本: `{}` · 消息类型数: {}\n\n",
        WireProtocol::kVersion,
        samples.size()
    );
    out += "字段类型按序列化后的 JSON 取值推导 (`array<T>` 表示数组, `object{...}` 展开一层\n";
    out += "字段); 省略的字段表示该字段可缺省 (取零值/默认值), 老对端不认识的新字段会被忽略。\n\n";

    out += "| # | 消息 | `type` | 字段 (JSON 类型) |\n";
    out += "|---:|---|---|---|\n";
    for (size_t i = 0; i < samples.size(); ++i) {
        const auto& sample = samples[i];
        const auto  json   = utilxx_base::Json::parse(serialize(sample.message));
        std::string fields;
        for (const auto& [key, value] : json.items()) {
            if (!fields.empty()) {
                fields += ", ";
            }
            fields += fmt::format("`{}` {}", key, jsonTypeOf(value));
        }
        out += fmt::format(
            "| {} | {} | `{}` | {} |\n",
            i,
            sample.name,
            json.value("type", std::string{}),
            fields
        );
    }

    out += "\n## 说明\n\n";
    out += "- 每个 `type` 对应 `WireMessage` 变体的一个成员; 服务端按 `type` 分派, 未知\n";
    out += "  `type` 返回 `nullopt` (连接不断开)。\n";
    out += "- `error.code` 取值见 `WireErrorCode`: 0 internal / 1 invalid state / 2 session not\n";
    out += "  found / 3 session mismatch / 4 invalid args / 5 message not found; 未知码按 0 处理。\n";
    out += "- 连接阶段取值见 `WireConnectionStage`: unhandshaken / unbound / ready /\n";
    out += "  reconnecting / draining (未握手前只接受 `hello`)。\n";
    out += "- 心跳 (`ping` / `pong`) 是裸 JSON, 不进入 `WireMessage` 变体。\n";
    return out;
}

std::string schemaPath() {
#ifdef AGENTXX_WIRE_SCHEMA_PATH
    return std::string{AGENTXX_WIRE_SCHEMA_PATH};
#else
    return {};
#endif
}

std::string schemaDocPath() {
#ifdef AGENTXX_WIRE_SCHEMA_DOC_PATH
    return std::string{AGENTXX_WIRE_SCHEMA_DOC_PATH};
#else
    return {};
#endif
}


} // namespace

TestResult testWireSchema() {
    g_ws_passed = 0;
    g_ws_failed = 0;

    const auto samples = wireSamples();

    // 覆盖度: 每条消息一个示例, 顺序与变体一致 (新增消息类型忘了补示例时立即失败)
    XX_TEST_EXPECT_EQ(samples.size(), std::variant_size_v<WireMessage>);
    for (size_t i = 0; i < samples.size(); ++i) {
        XX_TEST_EXPECT_EQ(samples[i].message.index(), i);
    }

    // 序列化后必须带非空 type, 且互不重复 (type 是分派键)
    std::vector<std::string> typeTags;
    for (const auto& sample : samples) {
        const auto json = utilxx_base::Json::parse(serialize(sample.message));
        const auto tag  = json.value("type", std::string{});
        XX_TEST_EXPECT_TRUE(!tag.empty());
        auto parsed = agentxx::agent::io::deserialize(serialize(sample.message));
        XX_TEST_EXPECT_TRUE(parsed.has_value());
        if (parsed.has_value()) {
            // 反序列化必须回到同一变体成员
            XX_TEST_EXPECT_EQ(parsed->index(), sample.message.index());
        }
        for (const auto& seen : typeTags) {
            XX_TEST_EXPECT_TRUE(seen != tag);
        }
        typeTags.push_back(tag);
    }

    // 生成物比对 (缺失即失败; 更新模式写回; 比对实现见 schema_artifact.h)
    const auto jsonText = buildSchemaJson(samples);
    const auto docText  = buildSchemaDoc(samples);
    const bool update   = agentxx::test::artifactUpdateMode("AGENTXX_UPDATE_WIRE_SCHEMA");

    const auto jsonPath = schemaPath();
    if (!jsonPath.empty()) {
        const auto check = agentxx::test::checkGeneratedArtifact(
            jsonPath,
            jsonText,
            update,
            "wire-schema.json",
            "AGENTXX_UPDATE_WIRE_SCHEMA"
        );
        XX_TEST_EXPECT_TRUE(check.ok);
        if (!check.ok) {
            TEST_FAIL << check.message << std::endl;
        } else if (update) {
            TEST_INFO << check.message << std::endl;
        }
    }
    const auto docPath = schemaDocPath();
    if (!docPath.empty()) {
        const auto check = agentxx::test::checkGeneratedArtifact(
            docPath,
            docText,
            update,
            "wire-protocol-fields.md",
            "AGENTXX_UPDATE_WIRE_SCHEMA"
        );
        XX_TEST_EXPECT_TRUE(check.ok);
        if (!check.ok) {
            TEST_FAIL << check.message << std::endl;
        } else if (update) {
            TEST_INFO << check.message << std::endl;
        }
    }

    // 生成内容本身的形状断言 (路径未注入时也校验)
    XX_TEST_EXPECT_TRUE(jsonText.find("\"messages\"") != std::string::npos);
    XX_TEST_EXPECT_TRUE(docText.find("| # | 消息 |") != std::string::npos);

    return TestResult{g_ws_passed, g_ws_failed};
}

} // namespace test
} // namespace agentxx
