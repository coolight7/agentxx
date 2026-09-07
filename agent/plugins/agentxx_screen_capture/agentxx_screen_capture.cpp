/// agentxx_screen_capture —— 屏幕捕获插件 (Windows)
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

struct ScreenCapturePluginCtx;

struct ScreenCaptureHolder {
    bool startStreaming(int frameRate);
    void stopStreaming();

    agentxx_screen_capture_plugin::ScreenCapture capture_;
    ScreenCaptureScreenCapturePluginCtx*                      ctx = nullptr;
};

struct ScreenCapturePluginCtx : public agentxx::plugin::PluginBase {
    std::string                          captures_dir;
    std::unique_ptr<ScreenCaptureHolder> holder;
    ~ScreenCapturePluginCtx() override {
        if (holder) {
            holder->capture_.shutdown();
        }
    }
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
                    auto topicSv
                        = agentxx::plugin::PluginStringView::fromCstr("agentxx_screen_capture.frame"
                        );
                    auto payloadSv
                        = agentxx::plugin::PluginStringView::from(payload.data(), payload.size());
                    ctx->iface.events->publish(ctx->host, &topicSv, &payloadSv);
                }
            } catch (...) {
            }
        }
    );
}

inline void ScreenCaptureHolder::stopStreaming() {
    capture_.stopStreaming();
}

static auto ctxGuardLogger(ScreenCapturePluginCtx* ctx) noexcept {
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
#if XX_IS_WIN_D
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

static void registerScreenCaptureTool(ScreenCapturePluginCtx& ctx) {
    auto schema = ctx.schema("agentxx_screen_capture")
        .enumString("command", "Operation to perform: capture_all (default), capture_mouse, capture_screen, get_screen_count, start_streaming, stop_streaming.",
                    {"capture_all", "capture_mouse", "capture_screen", "get_screen_count", "start_streaming", "stop_streaming"})
        .integer("screen_index", "Optional 0-based screen index for capture_screen (or default capture when specified).")
        .integer("frame_rate", "Target frame rate (1-30) for start_streaming. Default: 5.", false, 5)
        .boolean("save_images", "Save each captured frame as a PNG file under the host dataDir 'captures/' directory and return its file path. Pixels never enter the conversation. Default: true.", false, true)
        .build();

    auto        p      = ctx.toolPrompt("agentxx_screen_capture");
    std::string depict = p.depict.empty() ? kScreenCaptureDefaultDepict : p.depict;

    agentxx::plugin::blocking_tool(
        ctx,
        "agentxx_screen_capture",
        depict,
        schema,
        [](ScreenCapturePluginCtx& c, std::string_view args_json) -> std::string {
            agentxx::plugin::ArgReader args(args_json);
            if (!args.ok()) {
                throw std::runtime_error(args.errorMessage());
            }

            ScreenCaptureHolder& capture = *c.holder;
            std::string command = args.value("command", std::string{});
            bool saveImages = args.value("save_images", true);
            int64_t idx = args.value("screen_index", int64_t{-1});
            int64_t fr = args.value("frame_rate", int64_t{5});

            if (command.empty()) {
                command = (idx >= 0) ? "capture_screen" : "capture_all";
            }

            if (command == "capture_all") {
                auto frames = capture.capture_.captureAll();
                return formatFramesJson(frames, c.captures_dir, saveImages);
            }

            if (command == "capture_screen") {
                int target = (idx >= 0) ? static_cast<int>(idx) : 0;
                auto frame = capture.capture_.captureScreen(target);
                if (!frame.has_value()) {
                    return R"({"ok":false,"error":"capture_screen failed: invalid screen index or capture error"})";
                }
                std::vector<agentxx_screen_capture_plugin::ScreenFrame> frames;
                frames.push_back(std::move(*frame));
                return formatFramesJson(frames, c.captures_dir, saveImages);
            }

            if (command == "capture_mouse") {
                auto frame = capture.capture_.captureScreenUnderMouse();
                if (!frame.has_value()) {
                    return R"({"ok":false,"error":"capture_mouse failed"})";
                }
                std::vector<agentxx_screen_capture_plugin::ScreenFrame> frames;
                frames.push_back(std::move(*frame));
                return formatFramesJson(frames, c.captures_dir, saveImages);
            }

            if (command == "get_screen_count") {
                return fmt::format(R"({{"ok":true,"count":{}}})", capture.capture_.getScreenCount());
            }

            if (command == "start_streaming") {
                int rate = std::clamp(static_cast<int>(fr), 1, 30);
                bool ok = capture.startStreaming(rate);
                return fmt::format(R"({{"ok":{},"streaming":true,"frame_rate":{}}})", ok ? "true" : "false", rate);
            }

            if (command == "stop_streaming") {
                capture.stopStreaming();
                return R"({"ok":true,"streaming":false})";
            }

            return R"({"ok":false,"error":"unknown command"})";
        },
        0,
        AGENTXX_PLUGIN_TOOL_FLAG_AUTO_SUMMARY
    );
}

} // namespace agentxx_screen_capture_plugin

AGENTXX_PLUGIN_AGENT_EXPORT(
    ScreenCapturePluginCtx,
    "agentxx_screen_capture",
    "1.0.0",
    "Screen capture and streaming on Windows (DXGI Desktop Duplication with GDI fallback)",
    [](ScreenCapturePluginCtx& ctx) -> int32_t {
        ctx.holder      = std::make_unique<ScreenCaptureHolder>();
        ctx.holder->ctx = &ctx;

        std::string cfgStr = ctx.config();
        if (!cfgStr.empty() && cfgStr != "{}") {
            try {
                auto j = neograph::json::parse(cfgStr);
                std::string dataDir = j.value("dataDir", std::string{});
                if (!dataDir.empty()) {
                    namespace fs = std::filesystem;
                    fs::path targetDir = fs::path(dataDir) / "captures";
                    std::error_code ec;
                    fs::create_directories(targetDir, ec);
                    if (!ec) {
                        ctx.captures_dir = targetDir.string();
                    }
                }
            } catch (...) {}
        }

        if (!ctx.iface.tools || !ctx.iface.tools->register_tool) {
            return -1;
        }

        registerScreenCaptureTool(ctx);
        return 0;
    }
);
