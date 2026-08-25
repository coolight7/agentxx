// agentxx_screen_capture —— 屏幕捕获插件 (Windows)
// - 从 agentxx_computer_use 拆分独立: 单帧/全部屏幕/鼠标屏/流式推送
// - 注册工具: agentxx_screen_capture (合并 capture_all / capture_mouse / capture_screen /
// get_screen_count / streaming)
// - 流式帧经 publish 事件推送 (topic "agentxx_screen_capture.frame")
// - 像素数据不进入会话消息: 捕获帧经 WIC 编码为 PNG 落盘到宿主 dataDir 的
//   captures/ 目录, 工具结果只包含元信息 + 文件路径 (消息保持 KB 级);
//   避免数 MB 的 base64 像素直接写入 tool 消息 (上下文爆炸/持久化放大/LLM 不可读)
// - 插件不链接 libagentxx: 描述经 get_tool_prompt 读取, 日志经 vtable log
#include "codegraph/core/json.hpp"
#include "fmt/format.h"
#include "screen_capture.h"
#include "agentxx/plugin/plugin_iface_helper.h"
#include "agentxx/plugin/plugin_tool_sync.h"
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
#include <string>
#include <vector>

namespace agentxx_screen_capture_plugin {

// =====================================================================
// 工具注册辅助
// =====================================================================

/// 读取宿主 toolPrompt 的 depict; 未配置返回空
static std::string readToolDepict(const std::string& toolName) {
    if (!g_host || !g_if.config || !g_if.config->get_tool_prompt) {
        return {};
    }
    char* json = g_if.config->get_tool_prompt(
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

    AgentxxSyncToolSpec spec{};
    spec.name        = agentxx_plugin_sv(name, std::strlen(name));
    spec.description = agentxx_plugin_sv(
        g_storage[g_storage.size() - 2].data(),
        g_storage[g_storage.size() - 2].size()
    );
    spec.parameters_json = agentxx_plugin_sv(g_storage.back().data(), g_storage.back().size());
    spec.user_data       = entryPtr;
    spec.flags           = flags;
    // 阻塞委托型: 屏幕捕获为慢同步操作 (offload 池线程执行)
    spec.execute         = +[](void*                   ud,
                       AgentxxPluginStringView args_json,
                       AgentxxPluginStringView,
                       AgentxxPluginStringView,
                       volatile int*,
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
    if (agentxx_register_sync_tool(g_host, &spec) != 0) {
        pluginLog(3, fmt::format("agentxx_screen_capture: register tool {} failed", name));
    }
}

// =====================================================================
// agentxx_screen_capture
// =====================================================================

// 前置声明 (ScreenCaptureHolder 流式回调引用; 定义见下方)
static codegraph::Json frameToJson(const agentxx::expand::ScreenFrame& f, bool saveImages);

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
        // 异常守卫: 帧回调运行在采集线程, 异常逃逸会 std::terminate 进程
        return capture_.startStreaming(
            rate,
            [](const std::vector<agentxx::expand::ScreenFrame>& frames) {
                try {
                    if (!g_host || !g_if.events || !g_if.events->publish) {
                        return;
                    }
                    codegraph::Json j = codegraph::Json::object();
                    j["frames"]       = codegraph::Json::array();
                    for (const auto& f : frames) {
                        // 流式帧只推元信息, 不落盘不携带像素
                        j["frames"].push_back(frameToJson(f, false));
                    }
                    std::string payload = j.dump();
                    g_if.events->publish(
                        g_host,
                        AGENTXX_SV("agentxx_screen_capture.frame"),
                        agentxx_plugin_sv(payload.data(), payload.size())
                    );
                } catch (...) {
                    pluginCatchLog("streaming frame publish");
                }
            }
        );
    }

    void stopStreaming() {
        capture_.stopStreaming();
    }

    agentxx::expand::ScreenCapture capture_;
};

/// 宿主通用配置缓存 (entry 时经 get_config 读取; get_config 仅 io 线程,
/// execute 运行在线程池, 因此只在装配期读取并缓存, 后续只读)
/// - capturesDir: 截图 PNG 落盘目录 {dataDir}/captures (空 = 禁用落盘)
static std::string g_capturesDir;

/// 生成截图文件路径: {capturesDir}/capture_{yyyyMMdd_HHmmss_mmm}_{screen}.png
static std::string buildCapturePath(int screenIndex) {
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
        g_capturesDir,
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

/// 屏幕帧 → 元信息 JSON (像素不入消息: 可选保存 PNG 文件, 返回文件路径)
/// - saveImages=true 且捕获目录可用时: 编码 PNG 落盘, 结果含 image_path;
///   编码/落盘失败在 image_error 标记, 不影响其余元信息返回
/// - 结果体积控制在 KB 级 (元信息仅数百字节), 不会污染会话上下文
static codegraph::Json frameToJson(const agentxx::expand::ScreenFrame& f, bool saveImages) {
    codegraph::Json j = codegraph::Json::object();
    j["width"]        = f.width;
    j["height"]       = f.height;
    j["offset_x"]     = f.offsetX;
    j["offset_y"]     = f.offsetY;
    j["screen_index"] = f.screenIndex;
    j["screen_name"]  = f.screenName;
    j["is_primary"]   = f.isPrimary;
    j["pixel_bytes"]  = static_cast<int64_t>(f.pixelData.size());
    if (saveImages && !f.pixelData.empty() && !g_capturesDir.empty()) {
        std::string path = buildCapturePath(f.screenIndex);
        if (ScreenCaptureHolder::instance().capture_.saveFramePng(f, path)) {
            j["image_path"]   = path;
            j["image_format"] = "png";
        } else {
            j["image_error"] = "failed to save png";
        }
    }
    return j;
}

/// 帧数组 → JSON (空帧标记失败)
static std::string
    framesResult(const std::vector<agentxx::expand::ScreenFrame>& frames, bool saveImages) {
    if (frames.empty()) {
        return R"({"ok":false,"error":"capture failed"})";
    }
    codegraph::Json arr = codegraph::Json::array();
    for (const auto& f : frames) {
        arr.push_back(frameToJson(f, saveImages));
    }
    codegraph::Json j = codegraph::Json::object();
    j["ok"]           = true;
    j["frames"]       = arr;
    return j.dump();
}

static const char* kScreenCaptureDefaultDepict
    = "Capture screen frames or control streaming on Windows: capture all screens, mouse screen, or a specific screen; "
      "get screen count; start/stop streaming (streamed frames are pushed as plugin events to topic 'agentxx_screen_capture.frame'). "
      "Captured frames are saved as PNG files under the host dataDir 'captures/' directory; "
      "the result only contains frame metadata (size/offset/screen) plus the image file path — "
      "pixel data never enters the conversation.";

static void registerScreenCaptureTool() {
    codegraph::Json cmd = codegraph::Json::object();
    cmd["type"]         = "string";
    cmd["description"]
        = "Operation to perform: capture_all (default), capture_mouse, capture_screen, "
          "get_screen_count, start_streaming, stop_streaming.";
    cmd["enum"] = codegraph::Json::array(
        {codegraph::Json("capture_all"),
         codegraph::Json("capture_mouse"),
         codegraph::Json("capture_screen"),
         codegraph::Json("get_screen_count"),
         codegraph::Json("start_streaming"),
         codegraph::Json("stop_streaming")}
    );
    codegraph::Json schema               = codegraph::Json::object();
    schema["type"]                       = "object";
    schema["properties"]                 = codegraph::Json::object();
    schema["properties"]["command"]      = cmd;
    schema["properties"]["screen_index"] = codegraph::Json({
        {"type",        "integer"                                                               },
        {"description",
         "Optional 0-based screen index for capture_screen (or default capture when specified)."}
    });
    schema["properties"]["frame_rate"]   = codegraph::Json({
        {"type",        "integer"                                                  },
        {"description", "Target frame rate (1-30) for start_streaming. Default: 5."}
    });
    schema["properties"]["save_images"]  = codegraph::Json({
        {"type",        "boolean"                  },
        {"description",
         "Save each captured frame as a PNG file under the host dataDir "
           "'captures/' directory and return its file path. Pixels never "
           "enter the conversation. Default: true."}
    });

    registerTool(
        "agentxx_screen_capture",
        kScreenCaptureDefaultDepict,
        schema.dump(),
        [](SimpleJson& args) -> std::string {
            auto&       capture = ScreenCaptureHolder::instance();
            std::string command;
            bool        hasCommand = jsonGetString(args.doc().at_pointer("/command"), command);

            bool saveImages = true;
            jsonGetBool(args.doc().at_pointer("/save_images"), saveImages);

            int64_t idx    = -1;
            bool    hasIdx = jsonGetInt(args.doc().at_pointer("/screen_index"), idx);

            if (!hasCommand || command.empty() || command == "capture"
                || command == "capture_all") {
                if (!hasCommand && hasIdx && idx >= 0) {
                    command = "capture_screen";
                } else {
                    return framesResult(capture.capture_.captureAllScreens(), saveImages);
                }
            }

            if (command == "capture_all") {
                return framesResult(capture.capture_.captureAllScreens(), saveImages);
            }
            if (command == "capture_mouse") {
                std::vector<agentxx::expand::ScreenFrame> frames;
                auto                                      f = capture.capture_.captureMouseScreen();
                if (f.width > 0) {
                    frames.push_back(std::move(f));
                }
                return framesResult(frames, saveImages);
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
                std::vector<agentxx::expand::ScreenFrame> frames;
                auto f = capture.capture_.captureScreen(static_cast<int>(idx));
                if (f.width > 0 && f.height > 0) {
                    frames.push_back(std::move(f));
                }
                return framesResult(frames, saveImages);
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
        },
        // 防御: 结果意外超长 (如未来新增字段) 时自动经 share_store 截断压缩
        AGENTXX_TOOL_FLAG_AUTO_SUMMARY
    );
}

/// 把插件默认提示词写入宿主 toolPrompt (仅当宿主无该条目时; io 线程)
/// - 用户 yaml 覆盖早于插件加载 → get_prompt 已含覆盖 → 跳过 (尊重用户配置)
/// - 宿主未提供 get_prompt/set_prompt (旧宿主) → 跳过, registerTool 回退插件默认
static void ensureToolPromptInHost() {
    if (!g_host || !g_if.prompt || !g_if.prompt->get_prompt || !g_if.prompt->set_prompt) {
        return;
    }
    char* json = g_if.prompt->get_prompt(g_host);
    if (!json) {
        return;
    }
    std::string s{json};
    g_host->vtable->free(json);
    SimpleJson j(s);
    if (!j.ok()) {
        return;
    }

    codegraph::Json patch = codegraph::Json::object();
    patch["toolPrompt"]   = codegraph::Json::object();
    bool needUpdate       = false;

    if (j.doc().at_pointer("/toolPrompt/agentxx_screen_capture").error()) {
        codegraph::Json tp   = codegraph::Json::object();
        tp["depict"]         = std::string{kScreenCaptureDefaultDepict};
        codegraph::Json args = codegraph::Json::object();
        args["command"]
            = "Command to execute: capture_all (default), capture_mouse, capture_screen, "
              "get_screen_count, start_streaming, stop_streaming.";
        args["screen_index"]
            = "Optional 0-based screen index for capture_screen (or default capture when specified).";
        args["frame_rate"] = "Target frame rate for start_streaming (1-30). Default: 5.";
        args["save_images"]
            = "Save each captured frame as PNG under the host dataDir 'captures/' and "
              "return the file path (pixels never enter the conversation). Default: true.";
        tp["args"]                                    = args;
        patch["toolPrompt"]["agentxx_screen_capture"] = tp;
        needUpdate                                    = true;
    }

    if (needUpdate) {
        std::string payload = patch.dump();
        if (g_if.prompt->set_prompt(g_host, agentxx_plugin_sv(payload.data(), payload.size()))
            != 0) {
            pluginLog(3, "agentxx_screen_capture: set_prompt failed");
        }
    }
}

} // namespace agentxx_screen_capture_plugin

using namespace agentxx_screen_capture_plugin;

// =====================================================================
// 插件入口 (C ABI)
// =====================================================================

extern "C" AGENTXX_PLUGIN_EXPORT const AgentxxPluginInfo* agentxx_plugin_get_info(void) {
    // C ABI 边界异常守卫: 异常返回 NULL (宿主按"未导出"处理)
    return agentxx::plugin_guard::guardCall(
        pluginCatchLog,
        nullptr,
        [&]() -> const AgentxxPluginInfo* {
        static const AgentxxPluginInfo info{
            AGENTXX_PLUGIN_API_VERSION,
            AGENTXX_SV("agentxx_screen_capture"),
            AGENTXX_SV("1.0.0"),
            AGENTXX_SV(
                "Screen capture on Windows: all screens, mouse screen, specific screen, and streaming"
            ),
        };
        return &info;
    });
}

extern "C" AGENTXX_PLUGIN_EXPORT int
    agentxx_plugin_entry(const AgentxxHost* host, void** /*plugin_ctx*/) {
    // C ABI 边界异常守卫: entry 含目录创建/注册等可抛操作, 异常返回 -1
    return agentxx::plugin_guard::guardCall(
        pluginCatchLog,
        -1,
        [&]() -> int {
        g_host = host;
        // 读取宿主 dataDir (io 线程), 初始化截图落盘目录 {dataDir}/captures
        // - get_config 仅 io 线程可用, execute 运行在线程池, 故此处缓存供后续只读
        // - dataDir 不可用 (旧宿主) 时 g_capturesDir 为空, 捕获只返回元信息不落盘
        if (g_if.config && g_if.config->get_config) {
            char* json = g_if.config->get_config(g_host);
            if (json) {
                std::string s{json};
                g_host->vtable->free(json);
                SimpleJson j(s);
                if (j.ok()) {
                    std::string dataDir;
                    if (jsonGetString(j.doc().at_pointer("/dataDir"), dataDir) && !dataDir.empty()) {
                        g_capturesDir = dataDir + "/captures";
                        std::error_code ec;
                        if (std::filesystem::create_directories(g_capturesDir, ec) || !ec) {
                            pluginLog(
                                2,
                                fmt::format(
                                    "agentxx_screen_capture: capture dir ready: {}",
                                    g_capturesDir
                                )
                            );
                        } else {
                            pluginLog(
                                3,
                                fmt::format(
                                    "agentxx_screen_capture: create captures dir failed: {}",
                                    ec.message()
                                )
                            );
                            g_capturesDir.clear();
                        }
                    }
                }
            }
        }
        ensureToolPromptInHost();
        registerScreenCaptureTool();
        pluginLog(2, "agentxx_screen_capture loaded (1 tool)");
        return 0;
    });
}

extern "C" AGENTXX_PLUGIN_EXPORT void agentxx_plugin_unload(void* /*plugin_ctx*/) {
    // C ABI 边界异常守卫: 卸载回调异常不得外泄
    agentxx::plugin_guard::guardCallVoid(pluginCatchLog, [&] {
        // 在宿主 FreeLibrary 之前显式释放 DXGI/D3D11/GDI 资源。
        // 这些资源由本 DLL 内的函数级静态 (ScreenCaptureHolder) 持有, 若留到
        // 卸载时的静态析构中释放, D3D11 设备销毁会在 Windows loader lock 下执行,
        // 显卡驱动内部线程无法退出 → 主线程 GetExitCodeThread 无限自旋挂死
        // (实测 AMD atidxx64 100% 复现, 见 screen_capture.h shutdown 注释)。
        // 此处运行于宿主 unload 回调 (正常上下文, 无 loader lock), 静态析构时
        // 已无 GPU 资源可释放, 安全。
        // 注: 工具反注册无需在此调用 —— 宿主卸载流程先 detachAll 摘除全部注册,
        // 之后才调本回调, 手动 unregister 反而会因工具已不存在而告警。
        ScreenCaptureHolder::instance().capture_.shutdown();
        pluginLog(2, "agentxx_screen_capture unloaded");
    });
}
