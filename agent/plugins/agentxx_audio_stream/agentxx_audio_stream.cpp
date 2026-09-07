/// agentxx_audio_stream —— 音频流捕获插件
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

struct AudioStreamPluginCtx;

struct AudioStreamHolder {
    bool start(agentxx_audio_stream_plugin::AudioDataSource source, uint32_t targetProcessId);
    void stop();

    agentxx_audio_stream_plugin::AudioStream stream_;
    AudioStreamPluginCtx*                    ctx = nullptr;
};

struct AudioStreamPluginCtx : public agentxx::plugin::PluginBase {
    std::unique_ptr<AudioStreamHolder> holder;
    ~AudioStreamPluginCtx() override {
        if (holder) {
            holder->stop();
        }
    }
};

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
                ctx->jsonEscape(sourceName(data.source)),
                data.processId,
                ctx->jsonEscape(data.processName),
                tsMs,
                ctx->jsonEscape(toBase64(data.data))
            );
            auto topicSv
                = agentxx::plugin::PluginStringView::fromCstr("agentxx_audio_stream.audio");
            auto payloadSv
                = agentxx::plugin::PluginStringView::from(payload.data(), payload.size());
            ctx->iface.events->publish(ctx->host, &topicSv, &payloadSv);
        } catch (...) {
        }
    });
    return stream_.start(source, targetProcessId);
}

void AudioStreamHolder::stop() {
    stream_.stop();
    stream_.removeAllListeners();
}

} // namespace

AGENTXX_PLUGIN_AGENT_EXPORT(
    AudioStreamPluginCtx,
    "agentxx_audio_stream",
    "1.0.0",
    "System audio stream capture event stream (WASAPI loopback/mic on Windows)",
    [](AudioStreamPluginCtx& ctx) -> int32_t {
        ctx.holder      = std::make_unique<AudioStreamHolder>();
        ctx.holder->ctx = &ctx;

        auto schema = ctx.schema("agentxx_audio_stream")
            .enumString("command", "Operation command: start capturing, stop capturing, or query status.",
                        {"start", "stop", "status"}, /*required=*/true)
            .enumString("source", "Audio source to capture (default: system_output). Only applies to start command.",
                        {"system_output", "program_output", "microphone_input"}, false, "system_output")
            .integer("target_process_id", "Target PID for program_output mode (default: 0). Only applies to start command.", false, 0)
            .build();

        agentxx::plugin::blocking_tool(
            ctx,
            "agentxx_audio_stream",
            "Capture system audio or microphone stream on Windows (WASAPI). Audio frames are pushed as plugin events to topic 'agentxx_audio_stream.audio'. Supports start, stop, and status query.",
            schema,
            [](AudioStreamPluginCtx& c, std::string_view args_json) -> std::string {
                agentxx::plugin::ArgReader args(args_json);
                auto command = args.require<std::string>("command");
                if (!args.ok()) return args.errorMessage();

                AudioStreamHolder& holder = *c.holder;

                if (command == "start") {
                    std::string srcStr = args.value("source", "system_output");
                    int64_t pid = args.value("target_process_id", int64_t{0});
                    auto source = parseSource(srcStr);
                    bool ok = holder.start(source, static_cast<uint32_t>(pid));
                    return fmt::format(
                        R"({{"ok":{},"running":true,"source":"{}","process_id":{}}})",
                        ok ? "true" : "false",
                        sourceName(source),
                        pid
                    );
                }

                if (command == "stop") {
                    holder.stop();
                    return R"({"ok":true,"running":false})";
                }

                if (command == "status") {
                    bool running = holder.stream_.isRunning();
                    return fmt::format(
                        R"({{"ok":true,"running":{},"source":"{}"}})",
                        running ? "true" : "false",
                        sourceName(holder.stream_.currentSource())
                    );
                }

                return R"({"ok":false,"error":"unknown command"})";
            }
        );

        return 0;
    }
);
