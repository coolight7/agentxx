// agentxx_audio_stream —— 音频流捕获插件
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

agentxx_audio_stream_plugin::AudioDataSource parseSource(const std::string& s) {
    if (s == "program_output") {
        return agentxx_audio_stream_plugin::AudioDataSource::ProgramOutput;
    }
    if (s == "microphone_input") {
        return agentxx_audio_stream_plugin::AudioDataSource::MicrophoneInput;
    }
    return agentxx_audio_stream_plugin::AudioDataSource::SystemOutput;
}

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

struct PluginCtx;

struct AudioStreamHolder {
    bool start(agentxx_audio_stream_plugin::AudioDataSource source, uint32_t targetProcessId);
    void stop();

    agentxx_audio_stream_plugin::AudioStream stream_;
    PluginCtx*                               ctx = nullptr;
};

struct PluginCtx : public agentxx::plugin::PluginBase {
    std::unique_ptr<AudioStreamHolder> holder;
};

static auto ctxGuardLogger(PluginCtx* ctx) noexcept {
    return [ctx](const char* msg) noexcept {
        if (ctx) {
            ctx->log.error(msg ? msg : "");
        }
    };
}

std::string jsonEscape(const PluginCtx* ctx, const std::string& s) {
    if (!ctx || !ctx->host || !ctx->iface.json || !ctx->iface.json->json_escape || s.empty()) {
        return "\"\"";
    }
    AgentxxPluginString esc{nullptr, 0};
    auto                sSv = agentxx::plugin::PluginStringView::from(s.data(), s.size());
    ctx->iface.json->json_escape(ctx->host, &sSv, &esc);
    if (!esc.data) {
        return "\"\"";
    }
    std::string out{esc.data, static_cast<size_t>(esc.size)};
    agentxx::plugin::PluginString::free(ctx->host, &esc);
    return out;
}

bool AudioStreamHolder::start(
    agentxx_audio_stream_plugin::AudioDataSource source,
    uint32_t                                     targetProcessId
) {
    if (stream_.isRunning()) {
        return false;
    }
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
                agentxx::plugin::PluginStringView::fromCstr("agentxx_audio_stream.audio"),
                agentxx::plugin::PluginStringView::from(payload.data(), payload.size())
            );
        } catch (...) {
            if (ctx) {
                ctx->log.error("audio event publish failed");
            }
        }
    });
    return stream_.start(source, targetProcessId);
}

void AudioStreamHolder::stop() {
    stream_.stop();
    stream_.removeAllListeners();
}

} // namespace

extern "C" AGENTXX_PLUGIN_EXPORT const AgentxxPluginInfo* agentxx_plugin_agent_get_info(void) {
    static const AgentxxPluginInfo info{
        AGENTXX_PLUGIN_API_VERSION,
        0,
        agentxx::plugin::PluginStringView::fromCstr("agentxx_audio_stream"),
        agentxx::plugin::PluginStringView::fromCstr("1.0.0"),
        agentxx::plugin::PluginStringView::fromCstr(
            "System audio stream capture event stream (WASAPI loopback/mic on Windows)"
        ),
    };
    return &info;
}

extern "C" AGENTXX_PLUGIN_EXPORT int
    agentxx_plugin_agent_create(const AgentxxPluginHost* host, void** plugin_ctx) {
    PluginCtx* raw = nullptr;
    return agentxx::plugin::guardCall(
        [&raw](const char* msg) noexcept {
            ctxGuardLogger(raw)(msg);
        },
        -1,
        [&]() -> int {
            if (!host || !host->vtable || !plugin_ctx) {
                return -1;
            }
            auto ctx = std::make_unique<PluginCtx>();
            ctx->init(host);
            ctx->holder      = std::make_unique<AudioStreamHolder>();
            ctx->holder->ctx = ctx.get();
            raw              = ctx.get();

            if (!ctx->iface.tools || !ctx->iface.tools->register_tool) {
                return -1;
            }

            agentxx::plugin::blocking_tool(
                *ctx,
                "agentxx_audio_stream",
                "Capture system audio or microphone stream on Windows (WASAPI). Audio frames are pushed as plugin events to topic 'agentxx_audio_stream.audio'. Supports start, stop, and status query.",
                R"({
  "type": "object",
  "properties": {
    "command": {
      "type": "string",
      "enum": ["start", "stop", "status"],
      "description": "Operation command: start capturing, stop capturing, or query status."
    },
    "source": {
      "type": "string",
      "enum": ["system_output", "program_output", "microphone_input"],
      "description": "Audio source to capture (default: system_output). Only applies to start command."
    },
    "target_process_id": {
      "type": "integer",
      "description": "Target PID for program_output mode (default: 0). Only applies to start command."
    }
  },
  "required": ["command"]
})",
                [](PluginCtx& c, std::string_view args_json) -> std::string {
                    std::string argsStr(
                        args_json.data() ? args_json.data() : "{}",
                        args_json.size()
                    );
                    SimpleJson args(argsStr.empty() ? "{}" : argsStr);
                    if (!args.ok()) {
                        throw std::runtime_error("invalid args json");
                    }

                    std::string command;
                    jsonGetString(args.doc().at_pointer("/command"), command);

                    AudioStreamHolder& holder = *c.holder;

                    if (command == "start") {
                        std::string srcStr = "system_output";
                        jsonGetString(args.doc().at_pointer("/source"), srcStr);
                        int64_t pid64 = 0;
                        jsonGetInt(args.doc().at_pointer("/target_process_id"), pid64);

                        auto source = parseSource(srcStr);
                        bool ok     = holder.start(source, static_cast<uint32_t>(pid64));
                        return fmt::format(R"({{"ok":{},"running":true}})", ok ? "true" : "false");
                    }

                    if (command == "stop") {
                        holder.stop();
                        return R"({"ok":true,"running":false})";
                    }

                    if (command == "status") {
                        bool running = holder.stream_.isRunning();
                        return fmt::format(
                            R"({{"ok":true,"running":{}}})",
                            running ? "true" : "false"
                        );
                    }

                    return R"({"ok":false,"error":"unknown command"})";
                }
            );

            *plugin_ctx = ctx.release();
            return 0;
        }
    );
}

extern "C" AGENTXX_PLUGIN_EXPORT void agentxx_plugin_agent_destroy(void* plugin_ctx) {
    auto* ctx = static_cast<PluginCtx*>(plugin_ctx);
    agentxx::plugin::guardCallVoid(ctxGuardLogger(ctx), [&] {
        if (ctx) {
            if (ctx->holder) {
                ctx->holder->stop();
            }
            delete ctx;
        }
    });
}
