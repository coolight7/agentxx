// agentxx_screen_capture —— 屏幕捕获插件 (Windows)
#include "fmt/format.h"
#include "screen_capture.h"
#include "screen_capture_plugin.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <functional>
#include <memory>
#include <neograph/json.h>
#include <string>
#include <vector>

using namespace agentxx_screen_capture_plugin;

namespace agentxx_screen_capture_plugin {

struct PluginCtx;

struct ScreenCaptureHolder {
    bool startStreaming(int frameRate);
    void stopStreaming();

    agentxx_screen_capture_plugin::ScreenCapture capture_;
    PluginCtx*                                   ctx = nullptr;
};

struct PluginCtx : public agentxx::plugin::PluginBase {
    std::string                          captures_dir;
    PluginLogSink                        log_sink;
    std::unique_ptr<ScreenCaptureHolder> holder;
};

inline bool ScreenCaptureHolder::startStreaming(int frameRate) {
    if (!ctx || !ctx->host || !ctx->iface.events || !ctx->iface.events->publish) {
        return false;
    }
    return capture_.startStreaming(
        frameRate,
        [ctx = this->ctx](const std::vector<agentxx_screen_capture_plugin::ScreenFrame>& frames) {
            try {
                if (!ctx || !ctx->host || !ctx->iface.events || !ctx->iface.events->publish) {
                    return;
                }
                for (const auto& frame : frames) {
                    std::string payload = fmt::format(
                        R"({{"width":{},"height":{},"offset_x":{},"offset_y":{},"screen_index":{},"screen_name":"{}","is_primary":{},"pixel_bytes":{}}})",
                        frame.width,
                        frame.height,
                        frame.offsetX,
                        frame.offsetY,
                        frame.screenIndex,
                        frame.screenName,
                        frame.isPrimary ? "true" : "false",
                        frame.pixelData.size()
                    );
                    ctx->iface.events->publish(
                        ctx->host,
                        agentxx_plugin_sv_cstr("agentxx_screen_capture.frame"),
                        agentxx_plugin_sv(payload.data(), payload.size())
                    );
                }
            } catch (...) {
            }
        }
    );
}

inline void ScreenCaptureHolder::stopStreaming() {
    capture_.stopStreaming();
}

static auto ctxGuardLogger(PluginCtx* ctx) noexcept {
    return [ctx](const char* msg) noexcept {
        if (ctx) {
            ctx->log.error(msg ? msg : "");
        }
    };
}

static std::string buildCapturePath(PluginCtx& ctx, int screenIndex) {
    const auto now = std::chrono::system_clock::now();
    const auto tt  = std::chrono::system_clock::to_time_t(now);
    const auto millis
        = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count()
          % 1000;
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &tt);
#else
    localtime_r(&tt, &tm);
#endif
    return fmt::format(
        "{}/capture_{:04d}{:02d}{:02d}_{:02d}{:02d}{:02d}_{:03d}_{}.png",
        ctx.captures_dir,
        tm.tm_year + 1900,
        tm.tm_mon + 1,
        tm.tm_mday,
        tm.tm_hour,
        tm.tm_min,
        tm.tm_sec,
        static_cast<int>(millis),
        screenIndex
    );
}

static neograph::json frameToJson(
    PluginCtx&                                        ctx,
    const agentxx_screen_capture_plugin::ScreenFrame& f,
    bool                                              saveImages
) {
    neograph::json j  = neograph::json::object();
    j["width"]        = f.width;
    j["height"]       = f.height;
    j["offset_x"]     = f.offsetX;
    j["offset_y"]     = f.offsetY;
    j["screen_index"] = f.screenIndex;
    j["screen_name"]  = f.screenName;
    j["is_primary"]   = f.isPrimary;
    j["pixel_bytes"]  = static_cast<int64_t>(f.pixelData.size());
    if (saveImages && !f.pixelData.empty() && !ctx.captures_dir.empty()) {
        std::string path = buildCapturePath(ctx, f.screenIndex);
        if (ctx.holder->capture_.saveFramePng(f, path)) {
            j["image_path"]   = path;
            j["image_format"] = "png";
        } else {
            j["image_error"] = "failed to save png";
        }
    }
    return j;
}

static std::string framesResult(
    PluginCtx&                                                     ctx,
    const std::vector<agentxx_screen_capture_plugin::ScreenFrame>& frames,
    bool                                                           saveImages
) {
    if (frames.empty()) {
        return R"({"ok":false,"error":"capture failed"})";
    }
    neograph::json arr = neograph::json::array();
    for (const auto& f : frames) {
        arr.push_back(frameToJson(ctx, f, saveImages));
    }
    neograph::json j = neograph::json::object();
    j["ok"]          = true;
    j["frames"]      = arr;
    return j.dump();
}

static const char* kScreenCaptureDefaultDepict
    = "Capture screen frames or control streaming on Windows: capture all screens, mouse screen, or a specific screen; "
      "get screen count; start/stop streaming (streamed frames are pushed as plugin events to topic 'agentxx_screen_capture.frame'). "
      "Captured frames are saved as PNG files under the host dataDir 'captures/' directory; "
      "the result only contains frame metadata (size/offset/screen) plus the image file path — "
      "pixel data never enters the conversation.";

static void registerScreenCaptureTool(PluginCtx& ctx) {
    neograph::json cmd = neograph::json::object();
    cmd["type"]        = "string";
    cmd["description"]
        = "Operation to perform: capture_all (default), capture_mouse, capture_screen, "
          "get_screen_count, start_streaming, stop_streaming.";
    cmd["enum"] = neograph::json::array(
        {"capture_all",
         "capture_mouse",
         "capture_screen",
         "get_screen_count",
         "start_streaming",
         "stop_streaming"}
    );
    neograph::json schema                = neograph::json::object();
    schema["type"]                       = "object";
    schema["properties"]                 = neograph::json::object();
    schema["properties"]["command"]      = cmd;
    schema["properties"]["screen_index"] = neograph::json({
        {"type",        "integer"                                                               },
        {"description",
         "Optional 0-based screen index for capture_screen (or default capture when specified)."}
    });
    schema["properties"]["frame_rate"]   = neograph::json({
        {"type",        "integer"                                                  },
        {"description", "Target frame rate (1-30) for start_streaming. Default: 5."}
    });
    schema["properties"]["save_images"]  = neograph::json({
        {"type",        "boolean"                  },
        {"description",
         "Save each captured frame as a PNG file under the host dataDir "
           "'captures/' directory and return its file path. Pixels never "
           "enter the conversation. Default: true."}
    });

    auto        p      = ctx.toolPrompt("agentxx_screen_capture");
    std::string depict = p.depict.empty() ? kScreenCaptureDefaultDepict : p.depict;

    agentxx::plugin::blocking_tool(
        ctx,
        "agentxx_screen_capture",
        depict,
        schema.dump(),
        [](PluginCtx& c, std::string_view args_json) -> std::string {
            std::string argsStr(args_json.data() ? args_json.data() : "{}", args_json.size());
            SimpleJson  args(argsStr.empty() ? "{}" : argsStr);
            if (!args.ok()) {
                throw std::runtime_error("invalid args json");
            }

            ScreenCaptureHolder& capture = *c.holder;
            std::string          command;
            bool hasCommand = jsonGetString(args.doc().at_pointer("/command"), command);

            bool saveImages = true;
            jsonGetBool(args.doc().at_pointer("/save_images"), saveImages);

            int64_t idx    = -1;
            bool    hasIdx = jsonGetInt(args.doc().at_pointer("/screen_index"), idx);

            if (!hasCommand || command.empty() || command == "capture"
                || command == "capture_all") {
                if (!hasCommand && hasIdx && idx >= 0) {
                    command = "capture_screen";
                } else {
                    return framesResult(c, capture.capture_.captureAllScreens(), saveImages);
                }
            }

            if (command == "capture_all") {
                return framesResult(c, capture.capture_.captureAllScreens(), saveImages);
            }
            if (command == "capture_mouse") {
                std::vector<agentxx_screen_capture_plugin::ScreenFrame> frames;
                auto f = capture.capture_.captureMouseScreen();
                if (f.width > 0) {
                    frames.push_back(std::move(f));
                }
                return framesResult(c, frames, saveImages);
            }
            if (command == "capture_screen") {
                int screenCount = capture.capture_.getScreenCount();
                if (!hasIdx) {
                    idx = 0;
                }
                if (idx < 0 || idx >= screenCount) {
                    return fmt::format(
                        R"json({{"ok":false,"error":"screen index {} out of range (total screens: {})"}})json",
                        idx,
                        screenCount
                    );
                }
                std::vector<agentxx_screen_capture_plugin::ScreenFrame> frames;
                auto f = capture.capture_.captureScreen(static_cast<int>(idx));
                if (f.width > 0 && f.height > 0) {
                    frames.push_back(std::move(f));
                }
                return framesResult(c, frames, saveImages);
            }
            if (command == "get_screen_count") {
                neograph::json j = neograph::json::object();
                j["ok"]          = true;
                j["count"]       = capture.capture_.getScreenCount();
                return j.dump();
            }
            if (command == "start_streaming") {
                int64_t rate = 5;
                jsonGetInt(args.doc().at_pointer("/frame_rate"), rate);
                bool           ok = capture.startStreaming(static_cast<int>(rate));
                neograph::json j  = neograph::json::object();
                j["ok"]           = ok;
                j["rate"]         = rate;
                return j.dump();
            }
            if (command == "stop_streaming") {
                capture.stopStreaming();
                return R"({"ok":true})";
            }
            return R"({"ok":false,"error":"unknown command"})";
        },
        0,
        AGENTXX_PLUGIN_TOOL_FLAG_AUTO_SUMMARY
    );
}

} // namespace agentxx_screen_capture_plugin

extern "C" AGENTXX_PLUGIN_EXPORT const AgentxxPluginInfo* agentxx_plugin_agent_get_info(void) {
    static const AgentxxPluginInfo info{
        AGENTXX_PLUGIN_API_VERSION,
        agentxx_plugin_sv_cstr("agentxx_screen_capture"),
        agentxx_plugin_sv_cstr("1.0.0"),
        agentxx_plugin_sv_cstr(
            "Screen capture and streaming on Windows (DXGI Desktop Duplication with GDI fallback)"
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
            ctx->holder      = std::make_unique<ScreenCaptureHolder>();
            ctx->holder->ctx = ctx.get();
            raw              = ctx.get();

            ctx->log_sink = [raw = ctx.get()](int level, const std::string& msg) {
                if (raw) {
                    raw->log.log(level, msg);
                }
            };
            agentxx_screen_capture_plugin::g_log_sink.store(
                &ctx->log_sink,
                std::memory_order_release
            );

            if (ctx->iface.config && ctx->iface.config->get_config) {
                char* json = ctx->iface.config->get_config(ctx->host);
                if (json) {
                    std::string s{json};
                    ctx->host->vtable->free(json);
                    SimpleJson j(s);
                    if (j.ok()) {
                        std::string dataDir;
                        if (jsonGetString(j.doc().at_pointer("/dataDir"), dataDir)
                            && !dataDir.empty()) {
                            namespace fs              = std::filesystem;
                            fs::path        targetDir = fs::path(dataDir) / "captures";
                            std::error_code ec;
                            fs::create_directories(targetDir, ec);
                            if (!ec) {
                                ctx->captures_dir = targetDir.string();
                            }
                        }
                    }
                }
            }

            if (!ctx->iface.tools || !ctx->iface.tools->register_tool) {
                return -1;
            }

            registerScreenCaptureTool(*ctx);

            *plugin_ctx = ctx.release();
            return 0;
        }
    );
}

extern "C" AGENTXX_PLUGIN_EXPORT void agentxx_plugin_agent_destroy(void* plugin_ctx) {
    auto* ctx = static_cast<PluginCtx*>(plugin_ctx);
    agentxx::plugin::guardCallVoid(ctxGuardLogger(ctx), [&] {
        if (!ctx) {
            return;
        }
        if (ctx->holder) {
            ctx->holder->capture_.shutdown();
        }
        const auto* expected = &ctx->log_sink;
        agentxx_screen_capture_plugin::g_log_sink.compare_exchange_strong(expected, nullptr);
        delete ctx;
    });
}
