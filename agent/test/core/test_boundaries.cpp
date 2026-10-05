#include "agentxx-test/core/test_boundaries.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <regex>
#include <string>
#include <string_view>
#include <vector>

namespace {
// 本模块测试计数器 (仅本编译单元可见; 不经头文件 extern 导出)
int g_bd_passed = 0;
int g_bd_failed = 0;
} // namespace

// 断言计数宏覆盖: 将 test_framework.h 的 XX_TEST_EXPECT_* 映射到本模块计数器
#define XX_TEST_PASSED g_bd_passed
#define XX_TEST_FAILED g_bd_failed

namespace agentxx {
namespace test {

namespace fs = std::filesystem;

namespace {

/// 引用 (`#include`) 记录: 头名 + 行号, 用于失败时给出可定位信息
struct IncludeRef {
    std::string path;
    int         line = 0;
};

/// 发现的违规: 一段可直接打印的说明文本
using Violations = std::vector<std::string>;

/// 目录遍历时跳过的目录名
/// - build: 构建产物 (大量第三方源码副本)
/// - .git / .vscode: 版本库与编辑器配置
/// - third_party: 第三方依赖源码 (不属于本项目的边界约定对象)
bool isSkippedDir(std::string_view name) {
    return name == "build" || name == ".git" || name == ".vscode" || name == "third_party"
           || (!name.empty() && name.front() == '.');
}

/// 是否是需要检查的源文件后缀
bool isSourceFile(const fs::path& p) {
    const auto ext = p.extension().string();
    return ext == ".h" || ext == ".hpp" || ext == ".hxx" || ext == ".cpp" || ext == ".cc"
           || ext == ".cxx";
}

/// 递归收集目录下的源文件 (跳过 [isSkippedDir] 目录)
void collectSources(const fs::path& dir, std::vector<fs::path>& out) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        return;
    }
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (ec) {
            return;
        }
        const auto name = entry.path().filename().string();
        if (entry.is_directory(ec)) {
            if (!isSkippedDir(name)) {
                collectSources(entry.path(), out);
            }
        } else if (entry.is_regular_file(ec) && isSourceFile(entry.path())) {
            out.push_back(entry.path());
        }
    }
}

/// 读取文件中的 `#include` 指令 (支持 `#  include` 与两种括号)
std::vector<IncludeRef> readIncludes(const fs::path& file) {
    std::vector<IncludeRef> out;
    std::ifstream           ifs(file, std::ios::binary);
    if (!ifs) {
        return out;
    }
    static const std::regex kInclude{R"(^[ \t]*#[ \t]*include[ \t]+[<"]([^">]+)[">])"};
    std::string             line;
    int                     lineNo = 0;
    while (std::getline(ifs, line)) {
        ++lineNo;
        std::smatch m;
        if (std::regex_search(line, m, kInclude)) {
            out.push_back(IncludeRef{m[1].str(), lineNo});
        }
    }
    return out;
}

/// 读取整份文本 (失败返回空串)
std::string readTextFile(const fs::path& file) {
    std::ifstream ifs(file, std::ios::binary);
    if (!ifs) {
        return {};
    }
    return std::string{
        std::istreambuf_iterator<char>(ifs),
        std::istreambuf_iterator<char>()
    };
}

std::string toGeneric(const fs::path& p) {
    return p.generic_string();
}

/// 截断违规列表, 只打印前若干条, 末尾附总数
/// - 边界破坏往往成片出现 (一次误加头文件会牵出多处引用), 全量打印会淹没测试输出
void reportViolations(std::string_view rule, const Violations& v, size_t maxPrint = 20) {
    XX_TEST_EXPECT_EQ(v.size(), size_t{0});
    if (v.empty()) {
        return;
    }
    TEST_FAIL << "[" << rule << "] " << v.size() << " violation(s):" << std::endl;
    for (size_t i = 0; i < std::min(v.size(), maxPrint); ++i) {
        std::cout << "    " << v[i] << std::endl;
    }
    if (v.size() > maxPrint) {
        std::cout << "    ... (" << (v.size() - maxPrint) << " more)" << std::endl;
    }
}

/// agent 源码根目录 (即 `agent/`, 内含 lib/client/plugins/test)
/// - 由 `__FILE__` 推导: CMake 以绝对路径传入源文件, 故
///   `{agent}/test/core/test_boundaries.cpp` 去掉三层即得 `{agent}`
/// - 向上回溯校验目录特征 (lib+client+test 同时存在), 兼容不同构建布局;
///   找不到时返回空路径 (测试跳过并给出提示, 不误报失败)
fs::path agentSourceRoot() {
    std::error_code ec;
    auto            dir = fs::weakly_canonical(fs::path{__FILE__}, ec);
    if (ec) {
        dir = fs::path{__FILE__};
    }
    for (auto p = dir.parent_path(); !p.empty() && p != p.root_path(); p = p.parent_path()) {
        if (fs::is_directory(p / "lib" / "include", ec) && fs::is_directory(p / "client", ec)
            && fs::is_directory(p / "test", ec) && fs::is_directory(p / "plugins", ec)) {
            return p;
        }
    }
    return {};
}

// ---------------------------------------------------------------------------
// 规则实现
// ---------------------------------------------------------------------------

/// 规则 1/2: client 不得引用 lib 私有实现, 且 `agentxx/*` 引用必须落在公开 include 根
void checkClientIncludes(const fs::path& root, Violations& v, size_t& scanned) {
    std::vector<fs::path> files;
    collectSources(root / "client" / "include", files);
    collectSources(root / "client" / "src", files);

    const auto libIncRoot    = root / "lib" / "include";
    const auto clientIncRoot = root / "client" / "include";

    for (const auto& f : files) {
        ++scanned;
        for (const auto& inc : readIncludes(f)) {
            const auto where = toGeneric(f.lexically_relative(root)) + ":" + std::to_string(inc.line);
            if (inc.path.find("lib/src") != std::string::npos
                || inc.path.find("lib\\src") != std::string::npos) {
                v.push_back(where + ": client 引用 lib 私有实现头 `" + inc.path + "`");
                continue;
            }
            if (inc.path.rfind("agentxx/", 0) == 0) {
                std::error_code ec;
                if (!fs::exists(libIncRoot / inc.path, ec)
                    && !fs::exists(clientIncRoot / inc.path, ec)) {
                    v.push_back(
                        where + ": 引用的公开头不存在 `" + inc.path
                        + "` (应在 agent/lib/include 或 agent/client/include 下)"
                    );
                }
            }
        }
    }
}

/// 规则 3: 插件只引用 SDK 公开头与工具库公开头
///
/// 允许的宿主公开头 (插件 SDK 边界):
/// - `agentxx/plugin/api/*`: 插件 C ABI 头与宿主侧 kit (umbrella 为 plugin_kit.h)
/// - `agentxx/util/exception.h`: 异常分类与统一捕获 (插件错误上报依赖)
/// 其余 `agentxx/*` (agent / middlewares / nodes / tools / event / ui / protocol /
/// plugin 下的宿主内部头) 都属于宿主内部实现, 插件不得引用。
void checkPluginIncludes(const fs::path& root, Violations& v, size_t& scanned) {
    std::vector<fs::path> files;
    collectSources(root / "plugins", files);

    auto isAllowedHostHeader = [](std::string_view inc) {
        if (inc.rfind("agentxx/plugin/api/", 0) == 0) {
            return true;
        }
        return inc == "agentxx/util/exception.h";
    };

    for (const auto& f : files) {
        ++scanned;
        for (const auto& inc : readIncludes(f)) {
            const auto where = toGeneric(f.lexically_relative(root)) + ":" + std::to_string(inc.line);
            if (inc.path.find("lib/src") != std::string::npos
                || inc.path.find("lib\\src") != std::string::npos) {
                v.push_back(where + ": 插件引用 lib 私有实现头 `" + inc.path + "`");
                continue;
            }
            if (inc.path.rfind("agentxx/", 0) == 0 && !isAllowedHostHeader(inc.path)) {
                v.push_back(
                    where + ": 插件引用了 SDK 之外的宿主头 `" + inc.path
                    + "` (只允许 agentxx/plugin/api/* 与 agentxx/util/exception.h)"
                );
            }
        }
    }
}

/// 规则 4: lib 不得反向依赖 client (含 `agentxx-client/*` 与 client 目录内路径)
void checkLibIncludes(const fs::path& root, Violations& v, size_t& scanned) {
    std::vector<fs::path> files;
    collectSources(root / "lib", files);

    for (const auto& f : files) {
        ++scanned;
        for (const auto& inc : readIncludes(f)) {
            const auto where = toGeneric(f.lexically_relative(root)) + ":" + std::to_string(inc.line);
            if (inc.path.rfind("agentxx-client/", 0) == 0
                || inc.path.find("client/include") != std::string::npos
                || inc.path.find("client\\include") != std::string::npos
                || inc.path.find("client/src/") != std::string::npos) {
                v.push_back(where + ": lib 依赖了 client 侧头文件 `" + inc.path + "`");
            }
        }
    }
}

/// 规则 5: 插件动态库导出仍是入口白名单 (构建配置侧)
/// - ELF: -fvisibility=hidden + version script (global 白名单 + `local: *`)
/// - Mach-O: -exported_symbols_list
/// - MSVC: 不自动导出 (CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS 必须保持关闭)
void checkPluginExportConfig(const fs::path& root, Violations& v) {
    const auto cmakeFile  = root / "plugins" / "CMakeLists.txt";
    const auto cmakeText  = readTextFile(cmakeFile);
    const auto cmakeWhere = toGeneric(cmakeFile.lexically_relative(root));
    if (cmakeText.empty()) {
        v.push_back(cmakeWhere + ": 无法读取插件构建配置");
        return;
    }
    for (const std::string_view token :
         {"local: *", "--version-script", "exported_symbols_list"}) {
        if (cmakeText.find(token) == std::string::npos) {
            v.push_back(
                cmakeWhere + ": 缺少导出白名单配置 `" + std::string{token}
                + "` (插件只应导出 agentxx_plugin_{agent,client}_* 入口)"
            );
        }
    }
    static const std::regex kExportAll{R"(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS[ \t\r\n]+ON)"};
    if (std::regex_search(cmakeText, kExportAll)) {
        v.push_back(cmakeWhere + ": CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS 被打开 (会导出插件全部符号)");
    }
}

/// 规则 6: 插件源码不得手写平台导出属性 (必须经 SDK 的 PLUGINXX_EXPORT / AGENTXX_PLUGIN_*_EXPORT)
void checkPluginRawExport(const fs::path& root, Violations& v, size_t& scanned) {
    std::vector<fs::path> files;
    collectSources(root / "plugins", files);
    static const std::regex kRawExport{
        R"(__declspec[ \t]*\([ \t]*dllexport[ \t]*\)|__attribute__[ \t]*\(\([ \t]*visibility[ \t]*\([ \t]*"default")"
    };
    for (const auto& f : files) {
        ++scanned;
        const auto text = readTextFile(f);
        if (text.empty()) {
            continue;
        }
        const auto where = toGeneric(f.lexically_relative(root));
        std::string_view rest{text};
        int              lineNo = 0;
        size_t           pos    = 0;
        while (pos <= rest.size()) {
            const auto nl   = rest.find('\n', pos);
            const auto end  = (nl == std::string_view::npos) ? rest.size() : nl;
            const auto line = rest.substr(pos, end - pos);
            ++lineNo;
            if (std::regex_search(line.begin(), line.end(), kRawExport)) {
                v.push_back(
                    where + ":" + std::to_string(lineNo)
                    + ": 手写导出属性 (应使用 PLUGINXX_EXPORT 或 AGENTXX_PLUGIN_*_EXPORT 宏)"
                );
            }
            if (nl == std::string_view::npos) {
                break;
            }
            pos = nl + 1;
        }
    }
}

/// 规则 7: 组件渲染层只依赖"描述 + 渲染上下文"
/// - 渲染层 ([UiRenderCtx] / [renderItems] / 命中) 不得直接依赖连接、会话、
///   端点或插件管理器实现: 数据来源与连接状态由调用方组装后传入
/// - 只检查**直接** `#include` (间接引用由各自模块的规则约束: 插件管理器类型
///   经共享的 UI 注册表快照头引入, 属于允许的只读快照依赖)
/// - 文件缺失即失败: 改名/搬移后必须同步更新本规则, 不允许静默跳过
void checkRenderLayerIncludes(const fs::path& root, Violations& v) {
    const std::vector<fs::path> files{
        root / "client" / "include" / "agentxx-client" / "io" / "tui" / "ui_components.h",
        root / "client" / "src" / "io" / "tui" / "ui_components.cpp",
        root / "client" / "include" / "agentxx-client" / "io" / "tui" / "framework" / "ui_hit.h",
    };
    static const std::string_view kForbidden[] = {
        "agentxx-client/io/tui/agent_tui.h",      // TUI 端点 (连接/会话状态/事件循环)
        "agentxx-client/io/tui/components/",      // 具体界面部件 (持有会话镜像与端点)
        "agentxx/plugin/client_plugin_manager.h", // 插件管理器实现 (加载/生命周期)
        "agentxx/agent/io/",                      // 端点 / transport / wire
        "agentxx/agent/session",                  // 会话与持久化
        "agentxx/agent/agent_host.h",             // 宿主与子代理派生
        "utilxx/http_client.h",                   // 网络
        "utilxx/ws_client.h",                     // 网络
    };
    for (const auto& f : files) {
        std::error_code ec;
        if (!fs::is_regular_file(f, ec)) {
            v.push_back(
                "渲染层文件缺失: " + toGeneric(f.lexically_relative(root))
                + " (改名后需同步更新 boundaries 的渲染层规则)"
            );
            continue;
        }
        for (const auto& inc : readIncludes(f)) {
            const auto where = toGeneric(f.lexically_relative(root)) + ":" + std::to_string(inc.line);
            for (const auto& bad : kForbidden) {
                if (inc.path.rfind(bad, 0) == 0) {
                    v.push_back(
                        where + ": 渲染层依赖了 `" + inc.path + "` (只允许描述与渲染上下文)"
                    );
                }
            }
        }
    }
}

} // namespace

// ---------------------------------------------------------------------------
// 测试入口
// ---------------------------------------------------------------------------
TestResult testBoundaries() {
    const auto root = agentSourceRoot();
    if (root.empty()) {
        TEST_SKIP << "boundaries: agent 源码根目录未找到 (从 __FILE__ 推导失败), 跳过" << std::endl;
        return TestResult{0, 0};
    }
    TEST_INFO << "boundaries: agent 源码根 = " << toGeneric(root) << std::endl;

    size_t scannedClient = 0;
    size_t scannedPlugin = 0;
    size_t scannedLib    = 0;

    Violations clientViolations;
    checkClientIncludes(root, clientViolations, scannedClient);

    Violations pluginViolations;
    checkPluginIncludes(root, pluginViolations, scannedPlugin);

    Violations libViolations;
    checkLibIncludes(root, libViolations, scannedLib);

    Violations exportViolations;
    checkPluginExportConfig(root, exportViolations);
    checkPluginRawExport(root, exportViolations, scannedPlugin);

    Violations renderViolations;
    checkRenderLayerIncludes(root, renderViolations);

    // 扫描量下限: 防止目录改名/收集逻辑失效导致"零文件全通过"的假通过
    // (数值留出余量, 目录增删几十个文件不应触发失败)
    XX_TEST_EXPECT_GE(scannedClient, size_t{40});
    XX_TEST_EXPECT_GE(scannedPlugin, size_t{50});
    XX_TEST_EXPECT_GE(scannedLib, size_t{100});

    reportViolations("client 边界", clientViolations);
    reportViolations("插件边界", pluginViolations);
    reportViolations("lib 边界", libViolations);
    reportViolations("插件导出白名单", exportViolations);
    reportViolations("渲染层边界", renderViolations);

    TEST_INFO << "boundaries: scanned client=" << scannedClient << " plugin=" << scannedPlugin
              << " lib=" << scannedLib << " files" << std::endl;
    return TestResult{g_bd_passed, g_bd_failed};
}

} // namespace test
} // namespace agentxx
