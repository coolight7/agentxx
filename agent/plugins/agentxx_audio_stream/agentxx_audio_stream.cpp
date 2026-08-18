// agentxx_audio_stream —— 音频流捕获插件
// - 从 libagentxx src/expand/audio_stream 拆分独立: AudioStream (Windows
//   WASAPI 环回/麦克风捕获) 迁移为本插件内部实现
// - 注册工具: agentxx_audio_stream (start/stop/status)
// - 捕获到的音频数据经 publish 事件推送 (topic "agentxx_audio_stream.audio",
//   JSON: 元信息 + base64 PCM; 频率由捕获速率决定, 消费方自行订阅)
// - 非 Windows 平台 AudioStream 为 no-op (start 返回失败), 工具仍可查询状态
#include "audio_stream.h"
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
const char* sourceName(agentxx::expand::AudioDataSource s) {
    switch (s) {
        case agentxx::expand::AudioDataSource::SystemOutput:
            return "system_output";
        case agentxx::expand::AudioDataSource::ProgramOutput:
            return "program_output";
        case agentxx::expand::AudioDataSource::MicrophoneInput:
            return "microphone_input";
    }
    return "unknown";
}

/// 字符串 → 音频数据源枚举 (未知返回 system_output)
agentxx::expand::AudioDataSource parseSource(const std::string& s) {
    if (s == "program_output") {
        return agentxx::expand::AudioDataSource::ProgramOutput;
    }
    if (s == "microphone_input") {
        return agentxx::expand::AudioDataSource::MicrophoneInput;
    }
    return agentxx::expand::AudioDataSource::SystemOutput;
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

/// 转义字符串为 JSON 字面量 (经宿主 vtable json_escape)
std::string jsonEscape(const std::string& s) {
    if (!g_host || !g_host->vtable || !g_host->vtable->json_escape || s.empty()) {
        return "\"\"";
    }
    char* esc = g_host->vtable->json_escape(g_host, agentxx_plugin_sv(s.data(), s.size()));
    if (!esc) {
        return "\"\"";
    }
    std::string out{esc};
    g_host->vtable->free(esc);
    return out;
}

/// 音频捕获单例 (AudioStream 内部自管线程; unload 时停止)
struct AudioStreamHolder {
    static AudioStreamHolder& instance() {
        static AudioStreamHolder holder;
        return holder;
    }

    bool start(agentxx::expand::AudioDataSource source, uint32_t targetProcessId) {
        if (stream_.isRunning()) {
            return false;
        }
        stream_.addListener([](const agentxx::expand::AudioData& data) {
            if (!g_host || !g_host->vtable || !g_host->vtable->publish) {
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
                jsonEscape(sourceName(data.source)),
                data.processId,
                jsonEscape(data.processName),
                tsMs,
                jsonEscape(toBase64(data.data))
            );
            g_host->vtable->publish(
                g_host,
                AGENTXX_SV("agentxx_audio_stream.audio"),
                agentxx_plugin_sv(payload.data(), payload.size())
            );
        });
        return stream_.start(source, targetProcessId);
    }

    void stop() {
        stream_.stop();
        stream_.removeAllListeners();
    }

    agentxx::expand::AudioStream stream_;
};

/// 工具执行: command = start|stop|status
char* audioStreamExecute(
    void*                   user_data,
    AgentxxPluginStringView args_json,
    AgentxxPluginStringView thread_id,
    AgentxxPluginStringView tool_call_id,
    char**                  error_out
) {
    (void)user_data;
    (void)thread_id;
    (void)tool_call_id;
    try {
        std::string argsStr{args_json.data ? args_json.data : "{}", args_json.size};
        SimpleJson  args(argsStr.empty() ? "{}" : argsStr);
        if (!args.ok()) {
            throw std::runtime_error("invalid args json");
        }

        std::string command;
        jsonGetString(args.doc().at_pointer("/command"), command);

        auto& holder = AudioStreamHolder::instance();

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
                jsonEscape(sourceName(source))
            );
            return pluginStrdup(result.c_str());
        }

        if (command == "stop") {
            holder.stop();
            return pluginStrdup(R"({"ok":true,"running":false})");
        }

        if (command == "status") {
            bool running = holder.stream_.isRunning();
            return pluginStrdup(
                fmt::format(R"({{"ok":true,"running":{}}})", running ? "true" : "false").c_str()
            );
        }

        return pluginStrdup(R"({"ok":false,"error":"unknown command"})");
    } catch (const std::exception& ex) {
        if (error_out) {
            *error_out = pluginStrdup(ex.what());
        }
        return nullptr;
    } catch (...) {
        if (error_out) {
            *error_out = pluginStrdup("unknown exception");
        }
        return nullptr;
    }
}

} // namespace

// =====================================================================
// 插件入口 (C ABI)
// =====================================================================

extern "C" AGENTXX_PLUGIN_EXPORT const AgentxxPluginInfo* agentxx_plugin_get_info(void) {
    static const AgentxxPluginInfo info{
        AGENTXX_PLUGIN_API_VERSION,
        AGENTXX_SV("agentxx_audio_stream"),
        AGENTXX_SV("1.0.0"),
        AGENTXX_SV("Audio stream capture: system output / program output / microphone "
                   "(Windows WASAPI; other platforms no-op)"),
    };
    return &info;
}

extern "C" AGENTXX_PLUGIN_EXPORT int agentxx_plugin_entry(const AgentxxHost* host, void** /*plugin_ctx*/) {
    g_host = host;

    static const std::string kSchema = R"({
        "type": "object",
        "properties": {
            "command": {"type": "string", "enum": ["start", "stop", "status"]},
            "source": {"type": "string", "enum": ["system_output", "program_output", "microphone_input"]},
            "target_process_id": {"type": "integer", "description": "For program_output: capture only this process"}
        },
        "required": ["command"]
    })";

    AgentxxToolSpec spec{};
    spec.name = AGENTXX_SV("agentxx_audio_stream");
    spec.description
        = AGENTXX_SV("Capture audio stream on Windows: start/stop/status. Captured PCM frames are "
                     "published as plugin events (agentxx_audio_stream.audio).");
    spec.parameters_json = agentxx_plugin_sv(kSchema.data(), kSchema.size());
    spec.execute         = audioStreamExecute;
    if (host->vtable->register_tool(host, &spec) != 0) {
        pluginLog(3, "agentxx_audio_stream: register tool failed");
        return -1;
    }

    pluginLog(2, "agentxx_audio_stream loaded (1 tool)");
    return 0;
}

extern "C" AGENTXX_PLUGIN_EXPORT void agentxx_plugin_unload(void* /*plugin_ctx*/) {
    AudioStreamHolder::instance().stop();
    if (g_host && g_host->vtable) {
        g_host->vtable->unregister_tool(g_host, AGENTXX_SV("agentxx_audio_stream"));
    }
    pluginLog(2, "agentxx_audio_stream unloaded");
}
