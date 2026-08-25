// agentxx_audio_stream —— 音频流捕获插件
// - 从 libagentxx src/expand/audio_stream 拆分独立: AudioStream (Windows
//   WASAPI 环回/麦克风捕获) 迁移为本插件内部实现
// - 注册工具: agentxx_audio_stream (start/stop/status)
// - 捕获到的音频数据经 publish 事件推送 (topic "agentxx_audio_stream.audio",
//   JSON: 元信息 + base64 PCM; 频率由捕获速率决定, 消费方自行订阅)
// - 非 Windows 平台 AudioStream 为 no-op (start 返回失败), 工具仍可查询状态
#include "audio_stream.h"
#include "agentxx/plugin/plugin_iface_helper.h"
#include "audio_stream_plugin.h"
#include "fmt/format.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>

using namespace agentxx_audio_stream_plugin;

namespace {

/// 音频数据源枚举 → 字符串
const char* sourceName(agentxx_audio_stream_plugin::AudioDataSource s) {
    switch (s) {
        case agentxx_audio_stream_plugin::AudioDataSource::SystemOutput:
            return "system_output";
        case agentxx_audio_stream_plugin::AudioDataSource::ProgramOutput:
            return "program_output";
        case agentxx_audio_stream_plugin::AudioDataSource::MicrophoneInput:
            return "microphone_input";
    }
    return "unknown";
}

/// 字符串 → 音频数据源枚举 (未知返回 system_output)
agentxx_audio_stream_plugin::AudioDataSource parseSource(const std::string& s) {
    if (s == "program_output") {
        return agentxx_audio_stream_plugin::AudioDataSource::ProgramOutput;
    }
    if (s == "microphone_input") {
        return agentxx_audio_stream_plugin::AudioDataSource::MicrophoneInput;
    }
    return agentxx_audio_stream_plugin::AudioDataSource::SystemOutput;
}

/// PCM 字节 → base64 (捕获数据可能较大, 事件载荷为 JSON 字符串)
std::string toBase64(const std::vector<uint8_t>& data) {
    static const char* kBase64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string        b64;
    b64.reserve(((data.size() + 2) / 3) * 4);
    size_t i = 0;
    for (; i + 2 < data.size(); i += 3) {
        uint32_t n
            = (uint32_t{data[i]} << 16) | (uint32_t{data[i + 1]} << 8) | uint32_t{data[i + 2]};
        b64.push_back(kBase64[(n >> 18) & 63]);
        b64.push_back(kBase64[(n >> 12) & 63]);
        b64.push_back(kBase64[(n >> 6) & 63]);
        b64.push_back(kBase64[n & 63]);
    }
    if (i + 1 == data.size()) {
        uint32_t n = uint32_t{data[i]} << 16;
        b64.push_back(kBase64[(n >> 18) & 63]);
        b64.push_back(kBase64[(n >> 12) & 63]);
        b64.push_back('=');
        b64.push_back('=');
    } else if (i + 2 == data.size()) {
        uint32_t n = (uint32_t{data[i]} << 16) | (uint32_t{data[i + 1]} << 8);
        b64.push_back(kBase64[(n >> 18) & 63]);
        b64.push_back(kBase64[(n >> 12) & 63]);
        b64.push_back(kBase64[(n >> 6) & 63]);
        b64.push_back('=');
    }
    return b64;
}

/// 转义字符串为 JSON 字面量 (实现见 PluginCtx 完整定义后)
struct PluginCtx;

/// 音频捕获 (原函数级 static 单例在多实例下会把帧发到首实例宿主 —— 已修:
/// holder 移入每实例 PluginCtx, 监听回调捕获本实例裸指针)
struct AudioStreamHolder {
    bool start(agentxx_audio_stream_plugin::AudioDataSource source,
               uint32_t                                      targetProcessId); ///< 定义于 PluginCtx 完整声明后
    void stop();

    agentxx_audio_stream_plugin::AudioStream stream_;
    PluginCtx*                               ctx = nullptr; ///< 事件发布归属实例
};

/// 每实例上下文完整定义
struct PluginCtx {
    const AgentxxHost*                 host  = nullptr;
    agentxx::plugin::AgentIfaces       iface {};
    std::unique_ptr<AudioStreamHolder> holder; ///< 随实例生死 (destroy 先 stop)
    AgentxxSyncToolShim                shim {};///< 垫片适配器
};

/// 转义字符串为 JSON 字面量 (经宿主 vtable json_escape; host 取自本实例 ctx)
std::string jsonEscape(const PluginCtx* ctx, const std::string& s) {
    if (!ctx || !ctx->host || !ctx->iface.json || !ctx->iface.json->json_escape || s.empty()) {
        return "\"\"";
    }
    char* esc = ctx->iface.json->json_escape(ctx->host, agentxx_plugin_sv(s.data(), s.size()));
    if (!esc) {
        return "\"\"";
    }
    std::string out{esc};
    ctx->host->vtable->free(esc);
    return out;
}

bool AudioStreamHolder::start(agentxx_audio_stream_plugin::AudioDataSource source, uint32_t targetProcessId) {
        if (stream_.isRunning()) {
            return false;
        }
        // 异常守卫: 监听回调运行在音频捕获线程, 异常逃逸会 terminate 进程
        stream_.addListener([ctx = this->ctx](const agentxx_audio_stream_plugin::AudioData& data) {
            try {
                if (!ctx || !ctx->host || !ctx->iface.events || !ctx->iface.events->publish) {
                    return;
                }
                auto tsMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                data.timestamp.time_since_epoch()
                )
                                .count();
                std::string payload = fmt::format(
                    R"({{"sample_rate":{},"channels":{},"bits_per_sample":{},"source":{},"process_id":{},"process_name":{},"timestamp_ms":{},"data_base64":{}}})",
                    data.sampleRate,
                    data.channels,
                    data.bitsPerSample,
                    jsonEscape(ctx, sourceName(data.source)),
                    data.processId,
                    jsonEscape(ctx, data.processName),
                    tsMs,
                    jsonEscape(ctx, toBase64(data.data))
                );
                ctx->iface.events->publish(
                    ctx->host,
                    AGENTXX_SV("agentxx_audio_stream.audio"),
                    agentxx_plugin_sv(payload.data(), payload.size())
                );
            } catch (...) {
                pluginLog(ctx ? ctx->host : nullptr,
                          ctx ? ctx->iface.log : nullptr,
                          4,
                          "audio frame publish");
            }
        });
        return stream_.start(source, targetProcessId);
    }

void AudioStreamHolder::stop() {
        stream_.stop();
        stream_.removeAllListeners();
    }

/// 工具执行: command = start|stop|status (阻塞委托型; offload 池线程调用)
char* audioStreamExecute(
    void*                   user_data,
    AgentxxPluginStringView args_json,
    AgentxxPluginStringView thread_id,
    AgentxxPluginStringView tool_call_id,
    volatile int*           cancel_flag,
    char**                  error_out
) {
    auto* ctx    = static_cast<PluginCtx*>(user_data);
    auto* host   = ctx ? ctx->host : nullptr; ///< 多实例契约: 结果串走本实例宿主堆
    (void)thread_id;
    (void)tool_call_id;
    (void)cancel_flag;
    try {
        std::string argsStr{args_json.data ? args_json.data : "{}", args_json.size};
        SimpleJson  args(argsStr.empty() ? "{}" : argsStr);
        if (!args.ok()) {
            throw std::runtime_error("invalid args json");
        }

        std::string command;
        jsonGetString(args.doc().at_pointer("/command"), command);

        if (!ctx || !ctx->holder) {
            return pluginStrdup(host, R"({"ok":false,"error":"not initialized"})");
        }
        AudioStreamHolder& holder = *ctx->holder;

        if (command == "start") {
            std::string sourceStr;
            jsonGetString(args.doc().at_pointer("/source"), sourceStr);
            int64_t pid = 0;
            jsonGetInt(args.doc().at_pointer("/target_process_id"), pid);

            auto source = parseSource(sourceStr);
            bool ok     = holder.start(source, static_cast<uint32_t>(pid));
            auto result = fmt::format(
                R"({{"ok":{},"running":true,"source":{}}})",
                ok ? "true" : "false",
                jsonEscape(ctx, sourceName(source))
            );
            return pluginStrdup(host, result.c_str());
        }

        if (command == "stop") {
            holder.stop();
            return pluginStrdup(host, R"({"ok":true,"running":false})");
        }

        if (command == "status") {
            bool running = holder.stream_.isRunning();
            return pluginStrdup(
                host, fmt::format(R"({{"ok":true,"running":{}}})", running ? "true" : "false").c_str()
            );
        }

        return pluginStrdup(host, R"({"ok":false,"error":"unknown command"})");
    } catch (const std::exception& ex) {
        if (error_out) {
            *error_out = pluginStrdup(host, ex.what());
        }
        return nullptr;
    } catch (...) {
        if (error_out) {
            *error_out = pluginStrdup(host, "unknown exception");
        }
        return nullptr;
    }
}

} // namespace

// =====================================================================
// 插件入口 (C ABI)
// =====================================================================

extern "C" AGENTXX_PLUGIN_EXPORT const AgentxxPluginInfo* agentxx_plugin_get_info(void) {
    // C ABI 边界异常守卫: 异常返回 NULL; 本边界为纯静态元数据 → 空操作日志
    return agentxx::plugin_guard::guardCall(
        [](const char*) noexcept {},
        nullptr,
        [&]() -> const AgentxxPluginInfo* {
        static const AgentxxPluginInfo info{
            AGENTXX_PLUGIN_API_VERSION,
            AGENTXX_SV("agentxx_audio_stream"),
            AGENTXX_SV("1.0.0"),
            AGENTXX_SV("Audio stream capture: system output / program output / microphone "
                       "(Windows WASAPI; other platforms no-op)"),
        };
        return &info;
    });
}

extern "C" AGENTXX_PLUGIN_EXPORT int
    agentxx_plugin_create(const AgentxxHost* host, void** plugin_ctx) {
    // C ABI 边界异常守卫: 异常返回 -1 (创建失败); 日志闭包捕获局部裸指针
    auto ctx   = std::make_unique<PluginCtx>();
    PluginCtx* raw = nullptr;
    return agentxx::plugin_guard::guardCall(
        [&raw](const char* m) noexcept {
            pluginLog(raw ? raw->host : nullptr, raw ? raw->iface.log : nullptr, 4, m ? m : "");
        },
        -1,
        [&]() -> int {
        if (!host || !host->vtable || !plugin_ctx) {
            return -1;
        }
        ctx->host  = host;
        ctx->iface = agentxx::plugin::AgentIfaces::query(host);
        raw        = ctx.get();
        ctx->holder = std::make_unique<AudioStreamHolder>();
        ctx->holder->ctx = ctx.get(); ///< 事件回调经此读本实例宿主

        static const std::string kSchema = R"({
        "type": "object",
        "properties": {
            "command": {"type": "string", "enum": ["start", "stop", "status"]},
            "source": {"type": "string", "enum": ["system_output", "program_output", "microphone_input"]},
            "target_process_id": {"type": "integer", "description": "For program_output: capture only this process"}
        },
        "required": ["command"]
    })";

        AgentxxSyncToolSpec spec{};
        spec.name = AGENTXX_SV("agentxx_audio_stream");
        spec.description
            = AGENTXX_SV("Capture audio stream on Windows: start/stop/status. Captured PCM frames are "
                         "published as plugin events (agentxx_audio_stream.audio).");
        spec.parameters_json = agentxx_plugin_sv(kSchema.data(), kSchema.size());
        spec.user_data       = ctx.get();
        spec.execute         = audioStreamExecute;
        if (agentxx_register_sync_tool(host, &spec, &ctx->shim) != 0) {
            pluginLog(ctx->host, ctx->iface.log, 3,
                      "agentxx_audio_stream: register tool failed");
            return -1;
        }

        pluginLog(ctx->host, ctx->iface.log, 2, "agentxx_audio_stream loaded (1 tool)");
        *plugin_ctx = ctx.release(); ///< 所有权移交宿主 (destroy 时取回归还)
        return 0;
    });
}

extern "C" AGENTXX_PLUGIN_EXPORT void agentxx_plugin_destroy(void* plugin_ctx) {
    auto* ctx = static_cast<PluginCtx*>(plugin_ctx);
    // C ABI 边界异常守卫: 销毁回调异常不得外泄
    agentxx::plugin_guard::guardCallVoid(
        [ctx](const char* m) noexcept {
            pluginLog(ctx ? ctx->host : nullptr, ctx ? ctx->iface.log : nullptr, 4, m ? m : "");
        },
        [&] {
        if (!ctx) {
            return;
        }
        if (ctx->holder) {
            ctx->holder->stop(); ///< 先停捕获线程 (回调捕获 ctx, 必须先于 delete)
        }
        if (ctx->host && ctx->iface.tools && ctx->iface.tools->unregister_tool)
            ctx->iface.tools->unregister_tool(ctx->host, AGENTXX_SV("agentxx_audio_stream"));
        pluginLog(ctx->host, ctx->iface.log, 2, "agentxx_audio_stream unloaded");
        delete ctx;
        });
}
