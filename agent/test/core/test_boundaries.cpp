#include "agentxx-test/core/test_boundaries.h"

#include "agentxx/plugin/client_plugin_manager.h"
#include "agentxx/plugin/plugin_manager.h"
#include "pluginxx/ui/gen/blocks.g.h"
#include "utilxx_base/string_util.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <optional>
#include <regex>
#include <sstream>
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

/// 规则 9: 客户端声明的 UI 组件名与描述层组件表一致 (计划 TST-7)
///
/// 背景: 客户端只声明自己真的能画的组件 (能力段), 声明错名字的后果是三层
/// 静默失效 —— `adapt()` 认为客户端不支持该组件而提前降级, 渲染器里那条分支
/// 永远走不到, 插件按能力表以为可以拿到富组件。因此这里把"声明"与"描述层
/// 组件表"绑成一条门禁:
///   - 客户端能力数组 (`kTuiBlockNames`) 里每个名字都必须能在组件表里找到
///     (同大小写; 拼错 / 自造名字立即失败);
///   - 组件表里 `BlockLevel::Core` 的组件必须全部声明 (核心组件不支持等于
///     大面积降级, 属于实现缺口而不是取舍);
///   - 数组内不得有重复名字 (重复会让能力段 JSON 出现重复项)。
///
/// 只解析源码文本, 不依赖客户端是否参与本次构建 (避免关掉 client 后规则静默失效);
/// 文件或数组缺失即失败, 改名后必须同步更新本规则。
void checkTuiBlockNames(const fs::path& root, Violations& v) {
    const auto file = root / "client" / "src" / "io" / "tui" / "ui_components.cpp";
    const auto text = readTextFile(file);
    const auto where = toGeneric(file.lexically_relative(root));
    if (text.empty()) {
        v.push_back(where + ": 无法读取客户端组件能力表");
        return;
    }
    const auto keyPos = text.find("kTuiBlockNames");
    if (keyPos == std::string::npos) {
        v.push_back(where + ": 未找到 `kTuiBlockNames` (客户端能力表的唯一来源)");
        return;
    }
    const auto braceBegin = text.find('{', keyPos);
    const auto braceEnd   = (braceBegin == std::string::npos) ? std::string::npos
                                                              : text.find("};", braceBegin);
    if (braceBegin == std::string::npos || braceEnd == std::string::npos) {
        v.push_back(where + ": `kTuiBlockNames` 初始化列表无法解析 (改写后需同步本规则)");
        return;
    }
    static const std::regex kQuoted{R"re("([A-Za-z0-9_.]+)")re"};
    std::vector<std::string>  declared;
    const auto                body = text.substr(braceBegin, braceEnd - braceBegin);
    for (auto it = std::sregex_iterator(body.begin(), body.end(), kQuoted);
         it != std::sregex_iterator();
         ++it) {
        declared.push_back((*it)[1].str());
    }
    if (declared.empty()) {
        v.push_back(where + ": `kTuiBlockNames` 为空 (客户端必须至少支持 Text)");
        return;
    }

    const auto inTable = [](std::string_view name) {
        for (size_t i = 0; i < pluginxx::ui::gen::kBlockCount; ++i) {
            if (pluginxx::ui::gen::kBlockTable[i].kind == name) {
                return true;
            }
        }
        return false;
    };
    const auto isDeclared = [&declared](std::string_view name) {
        return std::find(declared.begin(), declared.end(), name) != declared.end();
    };

    // ① 声明即存在: 拼错 / 自造名字
    for (const auto& name : declared) {
        if (!inTable(name)) {
            v.push_back(
                where + ": 声明的组件 `" + name + "` 不在描述层组件表里 (名字拼写或已改名?)"
            );
        }
    }
    // ② 不得重复
    {
        auto sorted = declared;
        std::sort(sorted.begin(), sorted.end());
        for (auto it = std::adjacent_find(sorted.begin(), sorted.end()); it != sorted.end();
             it = std::adjacent_find(it + 1, sorted.end())) {
            v.push_back(where + ": 组件 `" + *it + "` 在能力表里重复出现");
        }
    }
    // ③ 核心组件必须全部声明
    for (size_t i = 0; i < pluginxx::ui::gen::kBlockCount; ++i) {
        const auto& meta = pluginxx::ui::gen::kBlockTable[i];
        if (meta.level == pluginxx::ui::BlockLevel::Core && !isDeclared(meta.kind)) {
            v.push_back(
                std::string{where} + ": 核心组件 `" + std::string{meta.kind}
                + "` 未在客户端能力表里声明 (核心组件不支持 = 大面积降级)"
            );
        }
    }
}

/// 规则 10: 接口表名集合 (计划 TST-7)
///
/// 数量常量 (`kInterfaceTableCount`) 只能发现"少了一张表", 发现不了"名字写错"或
/// "换了名字": 插件按 IID 查询, 名字错一个字就是查询失败。因此这里从 SDK 头文件
/// 里读出三组接口表的**名字**并校验:
///   - 通用表 (`pluginxx.*`, 由插件框架内核声明): 数量 + 唯一 + 前缀;
///   - agent 领域表 (`agentxx.agent.*`): 数量 + 唯一 + 前缀;
///   - client 侧表 (`agentxx.client.*`): 数量 + 唯一 + 前缀;
/// 同时要求 18 张领域表名都写进 `docs/zh-cn/design/plugins.md` (文档漏写新表
/// 等于插件作者查不到 IID)。
///
/// 数量口径: 10 张通用表 + 9 张 agent 领域表 = 19 (与 `PluginManager::kInterfaceTableCount`
/// 一致); 7 张 client 基础表 + 2 张交互表 (timer / keybind) = 9。
void checkInterfaceTableNames(const fs::path& root, Violations& v) {
    const auto genericHeader = root / "third_party" / "cxx_pluginxx" / "include" / "pluginxx"
                               / "api" / "tables.h";
    const auto agentHeader = root / "lib" / "include" / "agentxx" / "plugin" / "api"
                             / "plugin_api.h";
    const auto clientHeader = root / "lib" / "include" / "agentxx" / "plugin" / "api"
                              / "client_plugin_api.h";

    struct Group {
        const fs::path*  file;
        const char*      macro;  ///< 宏名前缀 (含 `_VERSION` 的变体不会匹配到引号, 天然排除)
        size_t           expect;
        const char*      prefix;
        bool             documented; ///< 是否要求写进 plugins.md
    };
    const Group groups[] = {
        {&genericHeader, "PLUGINXX_IFACE_", 10, "pluginxx.", false},
        {&agentHeader, "AGENTXX_PLUGIN_IFACE_AGENT_", 9, "agentxx.agent.", true},
        {&clientHeader, "AGENTXX_IFACE_CLIENT_", 9, "agentxx.client.", true},
    };

    std::vector<std::string> documentedNames;
    for (const auto& group : groups) {
        const auto text  = readTextFile(*group.file);
        const auto where = toGeneric(group.file->lexically_relative(root));
        if (text.empty()) {
            v.push_back(where + ": 无法读取接口表声明头 (接口表名门禁依赖它)");
            continue;
        }
        const std::regex pattern{
            std::string{R"re(^[ \t]*#[ \t]*define[ \t]+)re"} + group.macro
            + R"re([A-Z0-9_]+[ \t]+"([^"]+)")re"
        };
        std::vector<std::string> names;
        std::string_view         rest{text};
        size_t                   pos = 0;
        while (pos <= rest.size()) {
            const auto nl  = rest.find('\n', pos);
            const auto end = (nl == std::string_view::npos) ? rest.size() : nl;
            std::smatch m;
            const auto  line = std::string{rest.substr(pos, end - pos)};
            if (std::regex_search(line, m, pattern)) {
                names.push_back(m[1].str());
            }
            if (nl == std::string_view::npos) {
                break;
            }
            pos = nl + 1;
        }
        if (names.size() != group.expect) {
            v.push_back(
                where + ": 接口表数量为 " + std::to_string(names.size()) + ", 期望 "
                + std::to_string(group.expect)
                + " (增删接口表时同步 kInterfaceTableCount 与本规则)"
            );
        }
        auto sorted = names;
        std::sort(sorted.begin(), sorted.end());
        for (auto it = std::adjacent_find(sorted.begin(), sorted.end()); it != sorted.end();
             it = std::adjacent_find(it + 1, sorted.end())) {
            v.push_back(where + ": 接口表名重复 `" + *it + "`");
        }
        for (const auto& name : names) {
            if (name.rfind(group.prefix, 0) != 0) {
                v.push_back(
                    where + ": 接口表名 `" + name + "` 前缀不是 `" + group.prefix + "`"
                );
            }
            if (group.documented) {
                documentedNames.push_back(name);
            }
        }
    }

    const auto pluginsMd = root.parent_path() / "docs" / "zh-cn" / "design" / "plugins.md";
    const auto docText   = readTextFile(pluginsMd);
    if (docText.empty()) {
        v.push_back("缺少文档 `docs/zh-cn/design/plugins.md` (接口表名校验依赖它)");
        return;
    }
    for (const auto& name : documentedNames) {
        if (docText.find(name) == std::string::npos) {
            v.push_back("plugins.md 未列出接口表 `" + name + "` (插件作者查不到该 IID)");
        }
    }
}

/// 规则 11: 文档路径存在 (计划 TST-7)
///
/// `AGENTS.md` 是给人和 AI 看的入口, 里面引用的 `docs/zh-cn/**.md` 一旦改名或
/// 搬走就会变成死链 (读者点不开, 模型按旧路径找文档)。这里把根 `AGENTS.md` 与
/// 各目录 `AGENTS.md` 里出现的 `docs/zh-cn/**.md` 路径逐个按仓库根解析并检查
/// 存在性 —— 路径一律写仓库根相对路径 (与本项目其它文档引用一致)。
void checkDocumentedPaths(const fs::path& root, Violations& v) {
    const auto repoRoot = root.parent_path();
    std::vector<fs::path> agentsFiles{repoRoot / "AGENTS.md"};
    for (const char* sub : {"lib", "client", "plugins", "test"}) {
        agentsFiles.push_back(root / sub / "AGENTS.md");
    }

    static const std::regex kDocPath{R"(docs/zh-cn/[A-Za-z0-9_./-]+\.md)"};
    size_t                  totalRefs = 0;
    for (const auto& file : agentsFiles) {
        const auto text  = readTextFile(file);
        const auto where = toGeneric(file.lexically_relative(repoRoot));
        if (text.empty()) {
            v.push_back(where + ": 无法读取 (文档路径门禁依赖它)");
            continue;
        }
        std::error_code ec;
        for (auto it = std::sregex_iterator(text.begin(), text.end(), kDocPath);
             it != std::sregex_iterator();
             ++it) {
            const auto ref = (*it).str();
            ++totalRefs;
            if (!fs::is_regular_file(repoRoot / ref, ec)) {
                v.push_back(where + ": 引用的文档不存在 `" + ref + "` (改名后需同步引用)");
            }
        }
    }
    if (totalRefs == 0) {
        v.push_back("AGENTS.md 系列文件里没有解析到任何 `docs/zh-cn/**.md` 引用 (规则已失效?)");
    }
}

/// 规则 12: 客户端模型层只依赖数据与标准库 (计划 UI-1)
///
/// 背景: 历史分页窗口与消息队列镜像被抽成独立模型 (client/.../tui/model/),
/// 目的之一是**脱离终端可单测**。若模型里混进 FTXUI / 端点 / 传输 / 插件管理器
/// 头, 单测就得拉起终端与连接, 抽取的意义随之消失。这里按目录检查:
///   - `client/include/.../tui/model/*.h` 与 `client/src/io/tui/model/*.cpp`
///     不得包含 ftxui/*、agentxx-client/io/tui/agent_tui.h、components/、
///     agentxx/agent/io/(端点与 wire 之外的实现)、插件管理器实现等;
///   - 允许的类型来源: 标准库 + `agentxx/agent/io/agent_io_transport.h` 一类的
///     **协议数据结构** (模型要镜像 wire 上的队列/消息) + 描述层。
///
/// 目录为空同样判失败 (抽取被挪走/改名后规则不能静默失效)。
void checkModelLayerIncludes(const fs::path& root, Violations& v) {
    const auto includeDir = root / "client" / "include" / "agentxx-client" / "io" / "tui" / "model";
    const auto srcDir     = root / "client" / "src" / "io" / "tui" / "model";

    std::vector<fs::path> files;
    collectSources(includeDir, files);
    collectSources(srcDir, files);
    if (files.empty()) {
        v.push_back(
            "客户端模型层为空: "
            + toGeneric(includeDir.lexically_relative(root)) + " / "
            + toGeneric(srcDir.lexically_relative(root))
            + " (抽取被挪走后需同步更新本规则)"
        );
        return;
    }

    static const std::string_view kForbidden[] = {
        "ftxui/",                                  // 终端渲染框架
        "agentxx-client/io/tui/agent_tui.h",       // TUI 端点 (连接/会话状态/事件循环)
        "agentxx-client/io/tui/components/",       // 具体界面部件
        "agentxx-client/io/tui/framework/tui_state.h", // 渲染状态快照 (含组件状态)
        "agentxx-client/io/tui/ui_components.h",   // 渲染层
        "agentxx-client/io/tui/surface.h",
        "agentxx-client/plugin_ui_items.h",
        "agentxx/plugin/client_plugin_manager.h",  // 插件管理器实现
        "agentxx/agent/agent_host.h",              // 宿主与子代理派生
        "utilxx/http_client.h",                    // 网络
        "utilxx/ws_client.h",
    };
    for (const auto& f : files) {
        for (const auto& inc : readIncludes(f)) {
            const auto where = toGeneric(f.lexically_relative(root)) + ":" + std::to_string(inc.line);
            for (const auto& bad : kForbidden) {
                if (inc.path.rfind(bad, 0) == 0) {
                    v.push_back(
                        where + ": 模型层依赖了 `" + inc.path
                        + "` (模型只应依赖标准库与协议数据结构, 否则无法脱离终端单测)"
                    );
                }
            }
        }
    }
}

/// 规则 13: JSONL 一次性运行模式的 stdout 纪律 (计划 PRO-8)
///
/// 背景: `agentxx_cli jsonl` 的 stdout 就是对外协议 (每行一条 Wire 消息), 外部脚本按行
/// 解析。运行器里若混进直接打印 (调试输出/进度提示), stdout 会被污染, 脚本解析随之失败。
/// 因此本规则要求:
///   - `client/src/io/jsonl/jsonl_mode.cpp` 存在 (搬移/改名后必须同步本规则);
///   - 该文件里不出现直接写 stdout / stderr 的调用 (协议行由传输层写, 诊断走 XX_LOG*)
void checkJsonlModeStdoutDiscipline(const fs::path& root, Violations& v) {
    const auto      file = root / "client" / "src" / "io" / "jsonl" / "jsonl_mode.cpp";
    std::error_code ec;
    if (!fs::is_regular_file(file, ec)) {
        v.push_back(
            toGeneric(file.lexically_relative(root))
            + ": JSONL 运行模式实现缺失 (搬移后需同步更新本规则)"
        );
        return;
    }
    const auto text = readTextFile(file);
    if (text.empty()) {
        v.push_back(toGeneric(file.lexically_relative(root)) + ": 无法读取内容");
        return;
    }
    static const std::string_view kForbidden[] = {
        "std::cout",
        "std::cerr",
        "printf(",
        "fputs(",
        "putchar(",
    };
    const auto where = toGeneric(file.lexically_relative(root));
    for (const auto& bad : kForbidden) {
        if (text.find(bad) != std::string::npos) {
            v.push_back(
                where + ": 出现 `" + std::string{bad}
                + "` (stdout 只允许由 JsonlAgentIOTransport 写协议行, 诊断走 XX_LOG*)"
            );
        }
    }
}

} // namespace

/// 规则 8: 接口表数量与文档一致 (计划 PLG-8 / TST-7)
/// - 数量常量在 `PluginManager::kInterfaceTableCount` / `ClientPluginManager::kInterfaceTableCount`
/// - 文档: `docs/zh-cn/design/plugins.md` 与仓库根 `AGENTS.md` 都写明两侧数量;
///   增删接口表却忘记同步文档时本规则失败 (数字对不上就是漏更新)
void checkInterfaceTableCount(const fs::path& root, Violations& v) {
    const auto repoRoot  = root.parent_path();
    const auto pluginsMd = repoRoot / "docs" / "zh-cn" / "design" / "plugins.md";
    const auto agentsMd  = repoRoot / "AGENTS.md";

    const size_t agentCount  = agentxx::plugin::PluginManager::kInterfaceTableCount;
    const size_t clientCount = agentxx::plugin::ClientPluginManager::kInterfaceTableCount;

    // 数量本身: 10 张通用表 + 9 张 agent 领域表 / 7 张基础表 + 2 张交互表
    if (agentCount != 19) {
        v.push_back("PluginManager::kInterfaceTableCount 应为 19 (10 通用 + 9 领域), 实际 "
                    + std::to_string(agentCount));
    }
    if (clientCount != 9) {
        v.push_back("ClientPluginManager::kInterfaceTableCount 应为 9 (7 基础 + 2 交互), 实际 "
                    + std::to_string(clientCount));
    }

    const auto contains = [](const std::string& text, std::string_view needle) {
        return text.find(needle) != std::string::npos;
    };
    const auto readAll = [](const fs::path& p) -> std::optional<std::string> {
        std::ifstream in(utilxx_base::utf8ToPath(p.string()), std::ios::binary);
        if (!in) {
            return std::nullopt;
        }
        std::ostringstream buf;
        buf << in.rdbuf();
        return buf.str();
    };

    const auto pluginsText = readAll(pluginsMd);
    if (!pluginsText.has_value()) {
        v.push_back("缺少文档 `docs/zh-cn/design/plugins.md` (接口表数量校验依赖它)");
    } else {
        // 文档里两侧数量各出现一次口径: "19 张 agent" 与 "9 张 client"
        const std::string agentNeedle  = std::to_string(agentCount) + " 张 agent";
        const std::string clientNeedle = std::to_string(clientCount) + " 张 client";
        if (!contains(*pluginsText, agentNeedle)) {
            v.push_back("plugins.md 未写明 `" + agentNeedle + "` (接口表数量已变化?)");
        }
        if (!contains(*pluginsText, clientNeedle)) {
            v.push_back("plugins.md 未写明 `" + clientNeedle + "` (接口表数量已变化?)");
        }
    }

    const auto agentsText = readAll(agentsMd);
    if (!agentsText.has_value()) {
        v.push_back("缺少根 `AGENTS.md` (接口表数量校验依赖它)");
    } else {
        if (!contains(*agentsText, std::to_string(agentCount) + " 张")) {
            v.push_back("根 AGENTS.md 未写明 agent 侧接口表数量 `" + std::to_string(agentCount)
                        + " 张`");
        }
        if (!contains(*agentsText, std::to_string(clientCount) + " 张")) {
            v.push_back("根 AGENTS.md 未写明 client 侧接口表数量 `" + std::to_string(clientCount)
                        + " 张`");
        }
    }
}

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

    Violations ifaceViolations;
    checkInterfaceTableCount(root, ifaceViolations);
    checkInterfaceTableNames(root, ifaceViolations);

    Violations uiBlockViolations;
    checkTuiBlockNames(root, uiBlockViolations);

    Violations docPathViolations;
    checkDocumentedPaths(root, docPathViolations);

    Violations modelViolations;
    checkModelLayerIncludes(root, modelViolations);

    Violations jsonlViolations;
    checkJsonlModeStdoutDiscipline(root, jsonlViolations);

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
    reportViolations("接口表数量", ifaceViolations);
    reportViolations("UI 组件名", uiBlockViolations);
    reportViolations("文档路径", docPathViolations);
    reportViolations("客户端模型层边界", modelViolations);
    reportViolations("JSONL 模式 stdout 纪律", jsonlViolations);

    TEST_INFO << "boundaries: scanned client=" << scannedClient << " plugin=" << scannedPlugin
              << " lib=" << scannedLib << " files" << std::endl;
    return TestResult{g_bd_passed, g_bd_failed};
}

} // namespace test
} // namespace agentxx
