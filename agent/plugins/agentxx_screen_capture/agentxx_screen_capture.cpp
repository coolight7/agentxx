// agentxx_screen_capture —— 屏幕捕获插件 (Windows)
// - 从 agentxx_computer_use 拆分独立: 单帧/全部屏幕/鼠标屏/流式推送
// - 注册工具: agentxx_screen_capture
// - 流式帧经 publish 事件推送 (topic "agentxx_screen_capture.frame")
// - 插件不链接 libagentxx: 描述经 get_tool_prompt 读取, 日志经 vtable log
#include "codegraph/core/json.hpp"
#include "fmt/format.h"
#include "screen_capture.h"
#include "screen_capture_plugin.h"
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace agentxx_screen_capture_plugin {

// =====================================================================
// 工具注册辅助
// =====================================================================

/// 读取宿主 toolPrompt 的 depict; 未配置返回空
static std::string readToolDepict(const std::string& toolName) {
    if (!g_host || !g_host->vtable || !g_host->vtable->get_tool_prompt) {
        return {};
    }
    char* json = g_host->vtable->get_tool_prompt(
        g_host,
        agentxx_plugin_sv(toolName.data(), toolName.size())
    );
    if (!json) {
        return {};
    }
    std::string s{json};
    g_host->vtable->free(json);
    SimpleJson j(s);
    if (!j.ok()) {
        return {};
    }
    std::string depict;
    jsonGetString(j.doc().at_pointer("/depict"), depict);
    return depict;
}

/// 注册工具 (schema/描述存储于插件侧静态区)
/// - 静态存储: spec.execute 为静态 lambda (无捕获), fn 存于静态区
///   (unique_ptr 保证地址稳定), 经 user_data 传递; 插件生命周期内有效
/// - 字符串字段以 string_view 传入 (宿主注册时拷贝, 插件侧静态区存 std::string)
struct ToolEntry {
    std::function<std::string(SimpleJson&)> fn;
};

static void registerTool(
    const char*                             name,
    const char*                             defaultDepict,
    const std::string&                      schema,
    std::function<std::string(SimpleJson&)> fn,
    int                                     flags = 0
) {
    static std::vector<std::string> g_storage;
    std::string                     depict = readToolDepict(name);
    if (depict.empty()) {
        depict = defaultDepict;
    }
    g_storage.push_back(std::move(depict));
    g_storage.push_back(schema);

    static std::vector<std::unique_ptr<ToolEntry>> g_entries;
    auto                                           entry = std::make_unique<ToolEntry>();
    entry->fn                                            = std::move(fn);
    auto* entryPtr                                       = entry.get();
    g_entries.push_back(std::move(entry));

    AgentxxToolSpec spec{};
    spec.name        = agentxx_plugin_sv(name, std::strlen(name));
    spec.description = agentxx_plugin_sv(
        g_storage[g_storage.size() - 2].data(),
        g_storage[g_storage.size() - 2].size()
    );
    spec.parameters_json = agentxx_plugin_sv(g_storage.back().data(), g_storage.back().size());
    spec.user_data       = entryPtr;
    spec.flags           = flags;
    spec.execute         = +[](void*                   ud,
                       AgentxxPluginStringView args_json,
                       AgentxxPluginStringView,
                       AgentxxPluginStringView,
                       char** err) -> char* {
        auto* e = static_cast<ToolEntry*>(ud);
        try {
            std::string argsStr{args_json.data ? args_json.data : "{}", args_json.size};
            SimpleJson  args(argsStr.empty() ? "{}" : argsStr);
            if (!args.ok()) {
                throw std::runtime_error("invalid args json");
            }
            return pluginStrdup(e->fn(args).c_str());
        } catch (const std::exception& ex) {
            if (err) {
                *err = pluginStrdup(ex.what());
            }
            return nullptr;
        } catch (...) {
            if (err) {
                *err = pluginStrdup("unknown exception");
            }
            return nullptr;
        }
    };
    if (g_host->vtable->register_tool(g_host, &spec) != 0) {
        pluginLog(3, fmt::format("agentxx_screen_capture: register tool {} failed", name));
    }
}

// =====================================================================
// agentxx_screen_capture
// =====================================================================

/// 屏幕帧 → 元信息 JSON (像素可选 base64)
static codegraph::Json frameToJson(const agentxx::expand::ScreenFrame& f, bool includePixels) {
    codegraph::Json j = codegraph::Json::object();
    j["width"]        = f.width;
    j["height"]       = f.height;
    j["offset_x"]     = f.offsetX;
    j["offset_y"]     = f.offsetY;
    j["screen_index"] = f.screenIndex;
    j["screen_name"]  = f.screenName;
    j["is_primary"]   = f.isPrimary;
    j["pixel_bytes"]  = static_cast<int64_t>(f.pixelData.size());
    if (includePixels && !f.pixelData.empty()) {
        static const char* kBase64
            = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string b64;
        b64.reserve(((f.pixelData.size() + 2) / 3) * 4);
        size_t i = 0;
        for (; i + 2 < f.pixelData.size(); i += 3) {
            uint32_t n = (uint32_t{f.pixelData[i]} << 16) | (uint32_t{f.pixelData[i + 1]} << 8)
                         | uint32_t{f.pixelData[i + 2]};
            b64.push_back(kBase64[(n >> 18) & 63]);
            b64.push_back(kBase64[(n >> 12) & 63]);
            b64.push_back(kBase64[(n >> 6) & 63]);
            b64.push_back(kBase64[n & 63]);
        }
        if (i + 1 == f.pixelData.size()) {
            uint32_t n = uint32_t{f.pixelData[i]} << 16;
            b64.push_back(kBase64[(n >> 18) & 63]);
            b64.push_back(kBase64[(n >> 12) & 63]);
            b64.push_back('=');
            b64.push_back('=');
        } else if (i + 2 == f.pixelData.size()) {
            uint32_t n = (uint32_t{f.pixelData[i]} << 16) | (uint32_t{f.pixelData[i + 1]} << 8);
            b64.push_back(kBase64[(n >> 18) & 63]);
            b64.push_back(kBase64[(n >> 12) & 63]);
            b64.push_back(kBase64[(n >> 6) & 63]);
            b64.push_back('=');
        }
        j["pixels_base64"] = b64;
    }
    return j;
}

/// 帧数组 → JSON (空帧标记失败)
static std::string
    framesResult(const std::vector<agentxx::expand::ScreenFrame>& frames, bool includePixels) {
    if (frames.empty()) {
        return R"({"ok":false,"error":"capture failed"})";
    }
    codegraph::Json arr = codegraph::Json::array();
    for (const auto& f : frames) {
        arr.push_back(frameToJson(f, includePixels));
    }
    codegraph::Json j = codegraph::Json::object();
    j["ok"]           = true;
    j["frames"]       = arr;
    return j.dump();
}

/// 流式采集单例 (ScreenCapture 内部自管线程; unload 时停止)
struct ScreenCaptureHolder {
    static ScreenCaptureHolder& instance() {
        static ScreenCaptureHolder holder;
        return holder;
    }

    bool startStreaming(int rate) {
        if (capture_.isStreaming()) {
            return false;
        }
        rate = std::clamp(rate, 1, 30);
        return capture_.startStreaming(
            rate,
            [](const std::vector<agentxx::expand::ScreenFrame>& frames) {
                if (!g_host || !g_host->vtable || !g_host->vtable->publish) {
                    return;
                }
                codegraph::Json j = codegraph::Json::object();
                j["frames"]       = codegraph::Json::array();
                for (const auto& f : frames) {
                    j["frames"].push_back(frameToJson(f, false));
                }
                std::string payload = j.dump();
                g_host->vtable->publish(
                    g_host,
                    AGENTXX_SV("agentxx_screen_capture.frame"),
                    agentxx_plugin_sv(payload.data(), payload.size())
                );
            }
        );
    }

    void stopStreaming() {
        capture_.stopStreaming();
    }

    agentxx::expand::ScreenCapture capture_;
};

static void registerScreenCaptureTool() {
    codegraph::Json cmd = codegraph::Json::object();
    cmd["type"]         = "string";
    cmd["enum"]         = codegraph::Json::array(
        {codegraph::Json("capture_all"),
                 codegraph::Json("capture_mouse"),
                 codegraph::Json("capture_screen"),
                 codegraph::Json("get_screen_count"),
                 codegraph::Json("start_streaming"),
                 codegraph::Json("stop_streaming")}
    );
    codegraph::Json schema                 = codegraph::Json::object();
    schema["type"]                         = "object";
    schema["properties"]                   = codegraph::Json::object();
    schema["properties"]["command"]        = cmd;
    schema["properties"]["screen_index"]   = codegraph::Json({
        {"type", "number"}
    });
    schema["properties"]["frame_rate"]     = codegraph::Json({
        {"type", "number"}
    });
    schema["properties"]["include_pixels"] = codegraph::Json({
        {"type",        "boolean"                                            },
        {"description", "Include base64 pixel data (large!). Default: false."}
    });
    schema["required"]                     = codegraph::Json::array({codegraph::Json("command")});

    registerTool(
        "agentxx_screen_capture",
        "Capture screen frames on Windows: all screens, mouse screen, or a specific screen; "
        "also supports start/stop streaming (frames pushed as plugin events).",
        schema.dump(),
        [](SimpleJson& args) -> std::string {
            auto&       capture = ScreenCaptureHolder::instance();
            std::string command;
            jsonGetString(args.doc().at_pointer("/command"), command);
            bool includePixels = false;
            jsonGetBool(args.doc().at_pointer("/include_pixels"), includePixels);
            if (command == "capture_all") {
                return framesResult(capture.capture_.captureAllScreens(), includePixels);
            }
            if (command == "capture_mouse") {
                std::vector<agentxx::expand::ScreenFrame> frames;
                auto                                      f = capture.capture_.captureMouseScreen();
                if (f.width > 0) {
                    frames.push_back(std::move(f));
                }
                return framesResult(frames, includePixels);
            }
            if (command == "capture_screen") {
                int64_t idx = 0;
                jsonGetInt(args.doc().at_pointer("/screen_index"), idx);
                std::vector<agentxx::expand::ScreenFrame> frames;
                auto f = capture.capture_.captureScreen(static_cast<int>(idx));
                if (f.width > 0) {
                    frames.push_back(std::move(f));
                }
                return framesResult(frames, includePixels);
            }
            if (command == "get_screen_count") {
                codegraph::Json j = codegraph::Json::object();
                j["ok"]           = true;
                j["count"]        = capture.capture_.getScreenCount();
                return j.dump();
            }
            if (command == "start_streaming") {
                int64_t rate = 5;
                jsonGetInt(args.doc().at_pointer("/frame_rate"), rate);
                bool            ok = capture.startStreaming(static_cast<int>(rate));
                codegraph::Json j  = codegraph::Json::object();
                j["ok"]            = ok;
                j["rate"]          = rate;
                return j.dump();
            }
            if (command == "stop_streaming") {
                capture.stopStreaming();
                return R"({"ok":true})";
            }
            return R"({"ok":false,"error":"unknown command"})";
        }
    );
}

} // namespace agentxx_screen_capture_plugin

using namespace agentxx_screen_capture_plugin;

// =====================================================================
// 插件入口 (C ABI)
// =====================================================================

extern "C" const AgentxxPluginInfo* agentxx_plugin_get_info(void) {
    static const AgentxxPluginInfo info{
        AGENTXX_PLUGIN_API_VERSION,
        AGENTXX_SV("agentxx_screen_capture"),
        AGENTXX_SV("1.0.0"),
        AGENTXX_SV(
            "Screen capture on Windows: all screens, mouse screen, specific screen, and streaming"
        ),
    };
    return &info;
}

extern "C" int agentxx_plugin_entry(const AgentxxHost* host, void** /*plugin_ctx*/) {
    g_host = host;
    registerScreenCaptureTool();
    pluginLog(2, "agentxx_screen_capture loaded (1 tool)");
    return 0;
}

extern "C" void agentxx_plugin_unload(void* /*plugin_ctx*/) {
    ScreenCaptureHolder::instance().stopStreaming();
    pluginLog(2, "agentxx_screen_capture unloaded");
}
