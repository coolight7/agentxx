#include "agentxx/expand/codegraph_manager.h"
#include "agentxx/agent/config_static.h"
#include "agentxx/util/exception.h"
#include "agentxx/util/log.h"
#include "glob/glob.h"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <queue>
#include <regex>
#include <shared_mutex>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#if AGENTXX_ENABLE_CODEGRAPH
#include "codegraph/context/context_builder.h"
#include "codegraph/core/lru_cache.h"
#include "codegraph/core/types.h"
#include "codegraph/db/database.h"
#include "codegraph/extraction/extractor.h"
#include "codegraph/graph/traverser.h"
#include "codegraph/search/fts_search.h"
#include "codegraph/sync/file_watcher.h"

namespace agentxx {
namespace expand {

namespace fs = std::filesystem;

/// CodeGraph sqlite 数据库存放目录名: {dataDir}/sqlite/
static constexpr std::string_view kCodeGraphSqliteDirName = "sqlite";
/// CodeGraph 索引数据库子目录: {dataDir}/sqlite/codegraph/
static constexpr std::string_view kCodeGraphSqliteSubDirName = "codegraph";
/// 单个项目索引数据库文件名
static constexpr std::string_view kCodeGraphIndexDbName = "index.db";

/// 单段目录名最大长度 (截断后含分隔符与 hash 尾缀)
/// - Windows 默认 MAX_PATH=260 (未声明 longPathAware), 需控制单段长度与总层级
/// - Linux 单段 NAME_MAX=255, 整体 PATH_MAX=4096, 深度折叠后远低于限制
static constexpr size_t kCodeGraphMaxSegLen = 48;
/// 折叠后保留的尾部路径段数 (超过的部分折叠为 hash 前缀段)
/// - 最坏存储路径长度 ≈ 主目录(≤40) + 固定前缀(24) + 折叠段(16) + 3*48 + 分隔符 < 260
static constexpr size_t kCodeGraphMaxTailSegs = 3;

/// FNV-1a 64 位哈希 (截断用低 32 位 hex 输出)
/// - 仅用于超长段/折叠段的短标识, 确定性跨平台一致
static uint64_t fnv1a64(std::string_view s) {
    uint64_t hash = 1469598103934665603ULL;
    for (unsigned char c : s) {
        hash ^= c;
        hash *= 1099511628211ULL;
    }
    return hash;
}

/// CodeGraph sqlite 数据库目录: {dataDir}/sqlite/
/// - dataDir 为空时默认 ~/.agentxx/ (取不到用户主目录时回退系统临时目录)
/// - 可经 AgentConfig::dataDir / yaml data_dir 统一重定向
static std::string getCodeGraphSqliteDir(std::string_view sqliteDir) {
    if (!sqliteDir.empty()) {
        return std::string{sqliteDir};
    }
    return agentxx::agent::AgentConfigStatic::getSqliteDir("");
}

/// 路径段清洗: 替换文件系统非法字符为 `_`
/// - Windows 非法字符: < > : " / \ | ? * 及 ASCII 0-31
static std::string sanitizeSegment(std::string_view seg) {
    std::string out;
    out.reserve(seg.size());
    for (char c : seg) {
        if (static_cast<unsigned char>(c) < 0x20 || c == '<' || c == '>' || c == ':' || c == '"'
            || c == '/' || c == '\\' || c == '|' || c == '?' || c == '*') {
            out.push_back('_');
        } else {
            out.push_back(c);
        }
    }
    // 超长段截断: 保留前部可读信息 + 8 位 hex hash 尾缀防碰撞, 总长受控
    // - 避免单个目录名超过文件系统限制 (NAME_MAX=255) 及撑爆总路径长度
    if (out.size() > kCodeGraphMaxSegLen) {
        out = out.substr(0, kCodeGraphMaxSegLen - 9)
              + fmt::format("_{:08x}", static_cast<uint32_t>(fnv1a64(out) & 0xffffffffu));
    }
    return out;
}

/// Windows 保留设备名 (CON/PRN/AUX/NUL/COM1-9/LPT1-9, 忽略扩展名)
/// - 用作目录名会导致 Windows 无法创建, 需加前缀规避
#if XX_IS_WIN_D
static bool isWindowsReservedName(std::string_view seg) {
    std::string name{seg};
    auto        dot = name.find('.');
    if (dot != std::string::npos) {
        name = name.substr(0, dot);
    }
    for (auto& c : name) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    static const char* kReserved[] = {
        "CON",  "PRN",  "AUX",  "NUL",  "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7",
        "COM8", "COM9", "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9",
    };
    for (const auto* r : kReserved) {
        if (name == r) {
            return true;
        }
    }
    return false;
}
#endif

/// 将项目根目录转换为 sqlite 目录下的相对路径段序列
/// - Linux:   /home/user/proj     -> {home, user, proj}
/// - Windows: C:\Users\x\proj     -> {c, Users, x, proj}   (盘符去冒号转小写)
/// - UNC:     \\server\share\proj -> {server_share, proj}  (根名非法字符替换)
/// 返回完整路径段序列 (长度/深度控制见 foldSegments)
static std::vector<std::string> projectRootToSegments(std::string_view project_root) {
    std::vector<std::string> segs;

    // 绝对化并解析 `.` / `..` (weakly_canonical 不要求路径全部存在)
    fs::path abs;
    bool     ok = agentxx::util::catchError<bool>(
        [&]() -> bool {
            abs = fs::weakly_canonical(fs::path(project_root));
            return true;
        },
        [](std::string) -> bool {
            return false;
        }
    );
    if (!ok || abs.empty()) {
        abs = fs::absolute(fs::path(project_root));
    }

    // Windows 盘符 / UNC 根段
    std::string root_name = abs.root_name().string();
    if (!root_name.empty()) {
        std::string seg;
        for (char c : root_name) {
            // 去掉冒号与分隔符; 统一转小写 (盘符 C: -> c)
            if (c == ':' || c == '/' || c == '\\') {
                continue;
            }
            seg.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
        if (!seg.empty()) {
            segs.push_back(sanitizeSegment(seg));
        }
    }

    // 其余路径段
    for (const auto& part : abs.relative_path()) {
        auto seg = sanitizeSegment(part.string());
        if (seg.empty() || seg == ".") {
            continue;
        }
#if XX_IS_WIN_D
        if (isWindowsReservedName(seg)) {
            seg = "_" + seg;
        }
#endif
        segs.push_back(std::move(seg));
    }
    return segs;
}

/// 深度折叠: 段序列超过 [kCodeGraphMaxTailSegs] 时, 前缀部分折叠为 hash 段
/// - 折叠规则确定性: 相同前缀序列折叠结果一致, 保证前缀匹配可枚举
/// - 返回段数上限 = kCodeGraphMaxTailSegs + 1 (折叠段)
/// - 折叠段以 `_h` 开头, 与真实路径段 (sanitize 后) 冲突概率可忽略
static std::vector<std::string> foldSegments(const std::vector<std::string>& segs) {
    if (segs.size() <= kCodeGraphMaxTailSegs) {
        return segs;
    }
    std::vector<std::string> folded;
    // 折叠段: _h + 完整段序列中前缀部分的 hash (低 48 位 hex)
    std::string prefix;
    for (size_t i = 0; i + kCodeGraphMaxTailSegs < segs.size(); ++i) {
        if (!prefix.empty()) {
            prefix.push_back('/');
        }
        prefix += segs[i];
    }
    folded.push_back(
        fmt::format("_h{:012x}", static_cast<uint64_t>(fnv1a64(prefix) & 0xffffffffffffull))
    );
    for (size_t i = segs.size() - kCodeGraphMaxTailSegs; i < segs.size(); ++i) {
        folded.push_back(segs[i]);
    }
    return folded;
}

/// 项目根目录对应的索引数据库路径: {dataDir}/sqlite/codegraph/<折叠路径层级>/index.db
static fs::path getIndexDbPath(std::string_view project_root, std::string_view sqliteDir) {
    fs::path dir = fs::path(getCodeGraphSqliteDir(sqliteDir)) / kCodeGraphSqliteSubDirName;
    for (const auto& seg : foldSegments(projectRootToSegments(project_root))) {
        dir /= seg;
    }
    return dir / kCodeGraphIndexDbName;
}

/// 路径前缀匹配: 沿项目路径逐级向上查找最近已存在的索引数据库
/// - 例如工作目录为 /home/user/proj/sub 时, 若 /home/user/proj 已建索引则复用
/// - 每个前缀独立应用折叠规则 (确定性), 折叠后路径一致即视为同一前缀
/// - 未找到任何父级索引时返回 nullopt (将在自身路径下新建)
static std::optional<fs::path>
    findNearestExistingIndex(std::string_view project_root, std::string_view sqliteDir) {
    auto segs = projectRootToSegments(project_root);
    if (segs.empty()) {
        return std::nullopt;
    }
    fs::path base = fs::path(getCodeGraphSqliteDir(sqliteDir)) / kCodeGraphSqliteSubDirName;
    // 从最深 (最近) 前缀开始逐级缩短检查
    for (size_t n = segs.size(); n > 0; --n) {
        std::vector<std::string> prefix(
            segs.begin(),
            segs.begin() + static_cast<std::ptrdiff_t>(n)
        );
        fs::path dir = base;
        for (const auto& seg : foldSegments(prefix)) {
            dir /= seg;
        }
        std::error_code ec;
        if (fs::exists(dir / kCodeGraphIndexDbName, ec)) {
            return dir / kCodeGraphIndexDbName;
        }
    }
    return std::nullopt;
}

/// 带有限重试的操作执行: 应对多进程并发写同一 sqlite 库时的锁竞争 (SQLITE_BUSY)
/// - sqlite 的写锁竞争是瞬时的 (busy_timeout=5000 等待 + WAL 短事务), 重试后基本必成
/// - fn 抛异常时由内部 catchError 捕获, 按 attempt 指数退避后重试
/// - 全部尝试失败返回 false (已记录错误日志)
template<typename F>
static bool runWithRetry(std::string_view what, int attempts, F&& fn) {
    for (int attempt = 1; attempt <= attempts; ++attempt) {
        bool ok = agentxx::util::catchError<bool>(
            [&]() -> bool {
                fn();
                return true;
            },
            [&](std::string errmsg) -> bool {
                if (attempt < attempts) {
                    XX_LOGW(
                        "CodeGraphManager: {} failed (attempt {}/{}), retry: {}",
                        what,
                        attempt,
                        attempts,
                        errmsg
                    );
                    std::this_thread::sleep_for(std::chrono::milliseconds(150 * attempt));
                } else {
                    XX_LOGE(
                        "CodeGraphManager: {} failed after {} attempts: {}",
                        what,
                        attempts,
                        errmsg
                    );
                }
                return false;
            }
        );
        if (ok) {
            return true;
        }
    }
    return false;
}

/// 带有限重试的事务提交: BEGIN -> fn(写操作) -> COMMIT
/// - 写锁竞争 (SQLITE_BUSY) 时回滚并重试整个事务, 避免静默丢失该批写入
/// - fn 仅执行写操作, 不负责事务边界
template<typename F>
static bool
    runTransactionWithRetry(std::string_view what, int attempts, codegraph::Database* db, F&& fn) {
    for (int attempt = 1; attempt <= attempts; ++attempt) {
        bool inTx = false;
        bool ok   = agentxx::util::catchError<bool>(
            [&]() -> bool {
                db->begin_transaction();
                inTx = true;
                fn();
                db->commit();
                inTx = false;
                return true;
            },
            [&](std::string errmsg) -> bool {
                // 回滚需容错: 若 BEGIN 本身失败则无活跃事务, ROLLBACK 会再抛异常
                if (inTx) {
                    agentxx::util::catchError<bool>(
                        [&]() -> bool {
                            db->rollback();
                            return true;
                        },
                        [](std::string) -> bool {
                            return false;
                        }
                    );
                    inTx = false;
                }
                if (attempt < attempts) {
                    XX_LOGW(
                        "CodeGraphManager: {} failed (attempt {}/{}), retry: {}",
                        what,
                        attempt,
                        attempts,
                        errmsg
                    );
                    std::this_thread::sleep_for(std::chrono::milliseconds(150 * attempt));
                } else {
                    XX_LOGE(
                        "CodeGraphManager: {} failed after {} attempts: {}",
                        what,
                        attempts,
                        errmsg
                    );
                }
                return false;
            }
        );
        if (ok) {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// .gitignore / .gitmodules 规则解析与匹配
// ---------------------------------------------------------------------------

/// 单条 gitignore 规则 (已编译为正则)
struct GitIgnoreRule {
    std::string regex; // 完整匹配正则 (锚定规则针对 base 相对路径, 非锚定针对完整路径)
    bool       negate   = false;
    bool       anchored = false; // 包含 `/` 的模式锚定到规则所在目录
    fs::path   base;             // 规则所在目录 (anchored 时相对路径基准)
    std::regex re;               // 编译后的正则
};

/// gitignore 规则集合: 支持多级 .gitignore 继承 + 多级 .gitmodules 子模块目录
/// - 规则顺序: 根目录规则在前, 子目录规则在后; 匹配时从后往前取第一个命中
///   (gitignore 语义: 后出现的规则优先, 子目录规则覆盖父目录规则)
/// - 否定规则 (`!xxx`): 命中后取消忽略 (最后一个匹配规则决定最终结果)
/// - 目录命中忽略规则时, 其下所有内容一并忽略 (匹配正则隐含子树匹配)
/// - 内置规则: use_gitignore 启用时 `.git` 元数据目录 (任意层级, 含项目根下
///   的 .git) 整体忽略, 与 .gitignore/.gitmodules 规则一同生效
class GitIgnoreMatcher {
public:

    /// 添加内置忽略规则: `.git` 目录 (任意层级, 含项目根下的 .git) 整体忽略
    /// - 非锚定模式匹配任意层级; 幂等 (重复调用不重复添加规则)
    void addGitDirIgnore() {
        if (git_dir_ignored_) {
            return;
        }
        addPattern(".git", {});
        git_dir_ignored_ = true;
    }

    /// 解析并添加一个 .gitignore 文件 (不存在则忽略; base = 文件所在目录)
    void addIgnoreFile(const fs::path& ignore_file) {
        std::error_code ec;
        if (!fs::is_regular_file(ignore_file, ec)) {
            return;
        }
        std::ifstream ifs(ignore_file.string());
        if (!ifs.is_open()) {
            return;
        }
        fs::path    base = ignore_file.parent_path();
        std::string line;
        while (std::getline(ifs, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            addPattern(line, base);
        }
    }

    /// 解析 .gitmodules 中的子模块 `path = xxx` 并加入忽略 (子模块目录整体忽略)
    void addSubmodules(const fs::path& gitmodules_file) {
        std::error_code ec;
        if (!fs::is_regular_file(gitmodules_file, ec)) {
            return;
        }
        std::ifstream ifs(gitmodules_file.string());
        if (!ifs.is_open()) {
            return;
        }
        fs::path    base = gitmodules_file.parent_path();
        std::string line;
        while (std::getline(ifs, line)) {
            auto eq = line.find('=');
            if (eq == std::string::npos) {
                continue;
            }
            std::string key = line.substr(0, eq);
            std::string val = line.substr(eq + 1);
            trimStr(key);
            trimStr(val);
            if (key != "path" || val.empty()) {
                continue;
            }
            // 子模块目录整体忽略 (目录模式)
            addPattern(val + "/", base);
        }
    }

    /// 路径是否被忽略 (文件或目录; 目录命中即整棵子树忽略)
    bool isIgnored(const fs::path& p) const {
        if (rules_.empty()) {
            return false;
        }
        std::string path_str = p.generic_string();
        // 从后往前找第一个匹配规则 (最后一个匹配规则决定)
        for (auto it = rules_.rbegin(); it != rules_.rend(); ++it) {
            if (matchRule(*it, p, path_str)) {
                return !it->negate;
            }
        }
        return false;
    }

    bool empty() const {
        return rules_.empty();
    }

private:

    static void trimStr(std::string& s) {
        s.erase(0, s.find_first_not_of(" \t"));
        s.erase(s.find_last_not_of(" \t") + 1);
    }

    /// gitignore 模式 -> 正则片段 (不含锚定前后缀)
    /// - `**` 匹配任意字符 (含 `/`); `*` 匹配非 `/` 任意字符; `?` 匹配单个非 `/` 字符
    /// - `\x` 转义字面量
    static std::string toGitIgnoreRegex(std::string_view pattern) {
        std::string re;
        re.reserve(pattern.size() + 8);
        for (size_t i = 0; i < pattern.size(); ++i) {
            char c = pattern[i];
            if (c == '*') {
                // 连续 `*` 合并为一个 `**` (gitignore 中 `**` 可匹配 `/`)
                bool recursive = (i + 1 < pattern.size() && pattern[i + 1] == '*');
                while (i + 1 < pattern.size() && pattern[i + 1] == '*') {
                    ++i;
                }
                re += recursive ? ".*" : "[^/]*";
            } else if (c == '?') {
                re += "[^/]";
            } else if (c == '\\' && i + 1 < pattern.size()) {
                // 转义下一字符
                ++i;
                re += escapeRegexChar(pattern[i]);
            } else {
                re += escapeRegexChar(c);
            }
        }
        return re;
    }

    static std::string escapeRegexChar(char c) {
        static const std::string special = R"(\.^$+()[]{}|)";
        if (special.find(c) != std::string::npos) {
            return std::string("\\") + c;
        }
        return std::string(1, c);
    }

    /// 规则匹配: 锚定规则匹配 base 下的相对路径, 非锚定规则匹配任意层级
    static bool
        matchRule(const GitIgnoreRule& rule, const fs::path& p, const std::string& path_str) {
        if (rule.anchored) {
            std::error_code ec;
            auto            rel = fs::relative(p, rule.base, ec);
            if (ec || rel.empty()) {
                return false;
            }
            return std::regex_match(rel.generic_string(), rule.re);
        }
        // 非锚定: 模式可命中任意层级的同名条目, 路径开头通常不是模式本身
        // (如 Windows 盘符 C:/...), 必须用 regex_search 扫描任意位置;
        // 正则尾部的 `(?:/.*)?$` 保证命中后延伸到路径末尾 (目录命中即整棵子树忽略)
        return std::regex_search(path_str, rule.re);
    }

    void addPattern(std::string_view pattern, const fs::path& base) {
        // 空行 / 注释
        if (pattern.empty() || pattern[0] == '#') {
            return;
        }
        // 转义开头的 `\#` / `\!`
        if (pattern[0] == '\\' && pattern.size() >= 2 && (pattern[1] == '#' || pattern[1] == '!')) {
            pattern = pattern.substr(1);
        }
        GitIgnoreRule rule;
        rule.base = base;
        // 否定规则
        if (pattern[0] == '!') {
            rule.negate = true;
            pattern     = pattern.substr(1);
            if (pattern.empty()) {
                return;
            }
        }
        // 开头的 `/`: git 语义仅表示"锚定到规则所在目录", 不参与路径匹配,
        // 必须剥离, 否则正则以字面 `/` 开头, 无法匹配 relative() 得到的
        // 相对路径 (如 `.gitignore` 中的 `/third_party/boost*/`)
        bool anchored        = false;
        bool wildcard_prefix = false; // 以 `**/` 开头: 任意层级语义, 剩余含 `/` 也不锚定
        if (pattern.front() == '/') {
            anchored = true;
            pattern  = pattern.substr(1);
            if (pattern.empty()) {
                return;
            }
        }
        // `**/` 开头 (且非 `/` 开头): git 语义匹配任意层级 (如 `**/foo` ≡ `foo`,
        // `**/a/b` 匹配任意深度下的 a/b), 剥离 `**/` 并保持非锚定
        if (pattern.rfind("**/", 0) == 0) {
            wildcard_prefix = true;
            pattern         = pattern.substr(3);
            if (pattern.empty()) {
                return;
            }
        }
        // 结尾 `/` -> 仅目录 (匹配语义与普通模式一致, 正则隐含子树匹配)
        if (pattern.back() == '/') {
            pattern = pattern.substr(0, pattern.size() - 1);
            if (pattern.empty()) {
                return;
            }
        }
        // 剩余模式含 `/` (非开头非结尾, 且非 `**/` 开头) -> 锚定到规则所在目录
        if (!wildcard_prefix && pattern.find('/') != std::string_view::npos) {
            anchored = true;
        }
        rule.anchored = anchored;
        auto body     = toGitIgnoreRegex(pattern);
        if (rule.anchored) {
            // 匹配 base 下的相对路径本身或子树
            rule.regex = "^" + body + R"((?:/.*)?$)";
        } else {
            // 匹配任意层级的同名条目 (文件或目录) 及其子树
            rule.regex = R"((?:^|/))" + body + R"((?:/.*)?$)";
        }
        // 编译失败 (非法正则) 时丢弃该规则, 不中断解析
        bool compiled = agentxx::util::catchError<bool>(
            [&]() -> bool {
                rule.re = std::regex(rule.regex);
                return true;
            },
            [](std::string) -> bool {
                return false;
            }
        );
        if (compiled) {
            rules_.push_back(std::move(rule));
        }
    }

    std::vector<GitIgnoreRule> rules_;
    /// 内置 `.git` 忽略规则是否已添加 (addGitDirIgnore 幂等标记)
    bool git_dir_ignored_ = false;
};

/// gitignore 匹配器 (供单文件增量索引使用): 带目录级解析缓存
/// - 懒加载: 首次匹配某路径时, 沿祖先链向上解析各级 .gitignore 与 .gitmodules;
///   链在项目根处截止, 不读取项目根之外的规则 (避免项目外目录的
///   .gitignore/.gitmodules 干扰)
/// - 缓存: 已解析目录不再重复读盘; 锁保护, 与索引线程并发安全
/// - 失效: 每次 indexDirectory 前及 .gitignore/.gitmodules 文件变更事件时
///   由调用方 clear(), 保证新增/修改的规则及时生效
class GitIgnoreCache {
public:

    GitIgnoreCache() {
        // 内置规则: `.git` 目录 (任意层级) 整体忽略
        matcher_.addGitDirIgnore();
    }

    /// 设置项目根目录: 祖先链解析到项目根为止 (幂等, 可随时更新)
    void setRoot(fs::path root) {
        std::unique_lock<std::mutex> lock(mutex_);
        root_ = std::move(root);
    }

    /// 确保 path 祖先链上各级 .gitignore/.gitmodules 已解析 (幂等)
    void ensureLoaded(const fs::path& p) {
        std::unique_lock<std::mutex> lock(mutex_);
        // 收集祖先目录链 (叶 -> 根; 已知项目根时链在项目根处截止)
        std::vector<fs::path> dirs;
        fs::path              cur     = p.parent_path();
        fs::path              normRoot = normalizeDirPath(root_);
        while (!cur.empty()) {
            dirs.push_back(cur);
            if (!normRoot.empty() && normalizeDirPath(cur) == normRoot) {
                break; // 已到项目根: 不再向上读取项目外的规则
            }
            auto parent = cur.parent_path();
            if (parent == cur) {
                break;
            }
            cur = parent;
        }
        // 根 -> 叶 顺序解析 (保证规则顺序: 根规则在前)
        for (auto it = dirs.rbegin(); it != dirs.rend(); ++it) {
            if (loaded_dirs_.count(*it)) {
                continue;
            }
            // 每层目录追加本层 .gitignore 与 .gitmodules 规则 (父级规则保留,
            // 子目录规则追加在后, 匹配时后者优先; 嵌套 git 仓库的
            // .gitmodules 也在所在层级生效)
            matcher_.addIgnoreFile(*it / ".gitignore");
            matcher_.addSubmodules(*it / ".gitmodules");
            loaded_dirs_.insert(*it);
        }
    }

    bool isIgnored(const fs::path& p) {
        ensureLoaded(p);
        std::unique_lock<std::mutex> lock(mutex_);
        return matcher_.isIgnored(p);
    }

    void clear() {
        std::unique_lock<std::mutex> lock(mutex_);
        matcher_ = GitIgnoreMatcher{};
        matcher_.addGitDirIgnore();
        loaded_dirs_.clear();
        // 保留 root_: 项目根由 initialize 设置, 跨索引生命周期有效
    }

private:

    /// 归一化目录路径用于相等比较: lexically_normal 并去除尾部 `/`
    /// (Windows 盘符根 `C:/` 归一化后仍为自身, 不会死循环)
    static fs::path normalizeDirPath(const fs::path& p) {
        fs::path n = p.lexically_normal();
        if (!n.empty() && n.filename().empty()) {
            n = n.parent_path();
        }
        return n;
    }

    std::mutex                   mutex_;
    GitIgnoreMatcher             matcher_;
    std::unordered_set<fs::path> loaded_dirs_;
    /// 项目根目录 (祖先链解析的上限; 为空时不设上限)
    fs::path                     root_;
};

/// 路径字符串规范化: 统一 `/` 分隔符并 lexically_normal
/// - 配置文件中的路径可能是 Windows 反斜杠或含 `.`/`..` 段, 与遍历路径
///   (generic_string) 比较前统一
static std::string normalizePathStr(std::string_view p) {
    std::string s{p};
    std::replace(s.begin(), s.end(), '\\', '/');
    fs::path fp{s};
    return fp.lexically_normal().generic_string();
}

static bool should_skip(std::string_view file_path) {
    static const std::string kSkipAgentxx
        = fmt::format("/{}/", agentxx::agent::AgentConfigStatic::agentxxDataDirPath);

    fs::path    p(file_path);
    std::string path_str = p.generic_string();
    return path_str.find("/.") != std::string::npos
           || path_str.find("/node_modules/") != std::string::npos
           || path_str.find("/build/") != std::string::npos
           || path_str.find("/build-") != std::string::npos
           || path_str.find("/__pycache__/") != std::string::npos
           || path_str.find("/.git/") != std::string::npos
           || path_str.find(kSkipAgentxx) != std::string::npos;
}

/// 是否为 git 规则文件 (.gitignore / .gitmodules)
/// - 供文件监听识别: 规则文件变更时需失效 gitignore 规则缓存并重新解析
static bool isGitRuleFile(std::string_view path) {
    auto tail = [&](std::string_view name) {
        return path.size() >= name.size()
               && path.substr(path.size() - name.size()) == name;
    };
    return tail("/.gitignore") || tail("/.gitmodules");
}

/// 广度优先遍历根目录, 边遍历边回调每个待索引的源文件 (应用全部过滤规则)
/// @param ignore_path_regexes 配置 ignorePaths 编译后的正则列表
/// @param use_gitignore      是否启用 .gitignore/.gitmodules 忽略
/// @param on_file            每发现一个应索引的源文件调用一次 (调用方即时索引);
///                           返回 false 时停止遍历 (调用方主动中断, 不视为错误)
/// - 不再预先收集完整文件列表: 大目录 (数万文件) 下避免一次性列出全部路径
///   才返回 (阻塞索引与 UI 进度通知), 改为边遍历边回调, 索引进度/文件数
///   可随遍历逐渐增长
/// - 目录级剪枝: 命中忽略规则 (内置过滤/ignorePaths/gitignore) 的目录整棵子树
///   不进入遍历, 避免扫描 build/.git/third_party/子模块 等大目录
/// - gitignore 按层级继承: 每进入一层目录追加该层 .gitignore 与 .gitmodules
///   规则 (父级规则保留), 并内置忽略 `.git` 目录
/// - 显式栈遍历替代 recursive_directory_iterator, 避免深目录树递归栈溢出
static void traverse_source_files(
    std::string_view                   root_path,
    const std::vector<std::regex>&     ignore_path_regexes,
    bool                               use_gitignore,
    const std::function<bool(std::string_view)>& on_file
) {
    // 遍历异常记录日志后中止; 已回调的文件保持已处理状态 (不重复处理)
    agentxx::util::catchError<bool>(
        [&]() -> bool {
            GitIgnoreMatcher matcher;
            if (use_gitignore) {
                // 内置规则: `.git` 元数据目录 (任意层级, 含项目根下的 .git)
                // 整体忽略, 不进入索引
                matcher.addGitDirIgnore();
            }

            // 目录忽略判断 (剪枝): 内置过滤(补尾/使子串匹配命中目录自身) +
            // ignorePaths 正则 + gitignore
            auto isDirIgnored = [&](const fs::path& dir) -> bool {
                std::string s = dir.generic_string();
                if (should_skip(s + "/")) {
                    return true;
                }
                for (const auto& re : ignore_path_regexes) {
                    if (std::regex_match(s, re)) {
                        return true;
                    }
                }
                return matcher.isIgnored(dir);
            };
            // 文件忽略判断: 内置过滤 + ignorePaths 正则 + gitignore
            auto isFileIgnored = [&](const std::string& s) -> bool {
                if (should_skip(s)) {
                    return true;
                }
                for (const auto& re : ignore_path_regexes) {
                    if (std::regex_match(s, re)) {
                        return true;
                    }
                }
                return matcher.isIgnored(fs::path(s));
            };

            std::vector<fs::path> stack;
            stack.push_back(fs::path(root_path));
            while (!stack.empty()) {
                fs::path dir = std::move(stack.back());
                stack.pop_back();

                if (use_gitignore) {
                    // 每层目录追加本层 .gitignore 与 .gitmodules 规则 (父级
                    // 规则保留, 子目录规则追加在后, 匹配时后者优先; 嵌套 git
                    // 仓库的 .gitmodules 也在所在层级生效)
                    matcher.addIgnoreFile(dir / ".gitignore");
                    matcher.addSubmodules(dir / ".gitmodules");
                }

                std::error_code ec;
                for (auto it = fs::directory_iterator(
                         dir,
                         fs::directory_options::skip_permission_denied,
                         ec
                     );
                     it != fs::directory_iterator();
                     it.increment(ec)) {
                    if (ec) {
                        break;
                    }
                    const auto&     entry = *it;
                    fs::path        p     = entry.path();
                    std::error_code typeEc;
                    if (entry.is_directory(typeEc)) {
                        if (isDirIgnored(p)) {
                            continue; // 剪枝: 整棵子树忽略, 不进入
                        }
                        stack.push_back(std::move(p));
                    } else if (entry.is_regular_file(typeEc)) {
                        std::string path_str = p.generic_string();
                        if (isFileIgnored(path_str)) {
                            continue;
                        }
                        std::string lang = codegraph::detect_language(path_str);
                        if (lang.empty()) {
                            continue;
                        }
                        // 边遍历边回调 (调用方即时索引并通知进度; 返回 false 停止遍历)
                        if (!on_file(path_str)) {
                            return false; // 主动中断, 不视为错误
                        }
                    }
                }
            }
            return true;
        },
        [](std::string errmsg) -> bool {
            XX_LOGE("CodeGraphManager: traverse_source_files error: {}", errmsg);
            return false;
        }
    );
}

static bool is_changed(
    codegraph::Database&       db,
    const fs::directory_entry& entry,
    std::string_view           file_path
) {
    auto existing = db.get_file(std::string{file_path});
    if (!existing.has_value()) {
        return true;
    }
    // 无法读取文件元信息时按"已变更"处理, 触发重新索引
    return agentxx::util::catchError<bool>(
        [&]() -> bool {
            auto ftime = fs::last_write_time(entry);
            auto mtime = std::chrono::duration_cast<std::chrono::seconds>(ftime.time_since_epoch())
                             .count();
            return existing->mtime != mtime
                   || existing->size != static_cast<int64_t>(fs::file_size(entry));
        },
        [](std::string) -> bool {
            return true;
        }
    );
}

static int score_target(const codegraph::Node& source, const codegraph::Node& candidate) {
    int score = 0;
    if (source.file_path == candidate.file_path) {
        score += 10;
    } else {
        auto src_dir  = source.file_path.rfind('/');
        auto cand_dir = candidate.file_path.rfind('/');
        if (src_dir != std::string::npos && cand_dir != std::string::npos) {
            if (source.file_path.substr(0, src_dir) == candidate.file_path.substr(0, cand_dir)) {
                score += 5;
            }
        }
    }
    if (!source.qualified_name.empty() && !candidate.qualified_name.empty()) {
        auto src_colon  = source.qualified_name.rfind("::");
        auto cand_colon = candidate.qualified_name.rfind("::");
        if (src_colon != std::string::npos && cand_colon != std::string::npos) {
            std::string src_ns  = source.qualified_name.substr(0, src_colon);
            std::string cand_ns = candidate.qualified_name.substr(0, cand_colon);
            if (src_ns == cand_ns) {
                score += 3;
            }
        }
    }
    return score;
}

class CodeGraphManager::Impl {
public:

    /// @param sqliteDir sqlite 数据目录 (为空使用默认 {dataDir}/sqlite/)
    /// @param config    索引过滤配置 (加载路径/忽略路径/gitignore 开关)
    explicit Impl(std::string sqliteDir = "", CodeGraphIndexConfig config = {}) :
        running_(false),
        needs_initialize_(true),
        sqlite_dir_(std::move(sqliteDir)),
        index_config_(std::move(config)) {
        // 编译 ignorePaths 为正则 (支持 * 通配符):
        // - 含通配符: glob::to_regex (从宽: * 与 ** 均匹配任意字符含 /)
        // - 无通配符: 目录前缀匹配 (命中路径本身或其子树), 追加 (?:/.*)?
        // - 非法模式静默忽略, 不影响其他规则
        for (const auto& raw : index_config_.ignorePaths) {
            std::string p = normalizePathStr(raw);
            if (p.empty()) {
                continue;
            }
            std::string re = glob::to_regex(p);
            if (p.find_first_of("*?[") == std::string::npos) {
                re = re.substr(0, re.size() - 1) + R"((?:/.*)?$)";
            }
            agentxx::util::catchError<bool>(
                [&]() -> bool {
                    ignore_path_regexes_.emplace_back(re);
                    return true;
                },
                [](std::string) -> bool {
                    return false;
                }
            );
        }
    }

    ~Impl() {
        shutdown();
    }

    void shutdown() {
        if (running_.load()) {
            running_.store(false);
            cv_.notify_all();
            if (worker_thread_.joinable()) {
                worker_thread_.join();
            }
            file_watcher_.reset();
        }
        db_.reset();
        traverser_.reset();
        context_builder_.reset();
        fts_search_.reset();
        // 清空 gitignore 匹配缓存: 重新 initialize (可能换项目根) 时
        // 避免旧项目的 .gitignore 规则残留污染新项目
        gitignore_cache_.clear();
        needs_initialize_ = true;
    }

    bool initialize(std::string_view project_root) {
        {
            std::unique_lock<std::shared_mutex> lock(mutex_);
            if (!needs_initialize_ && project_root_ == project_root && db_) {
                return true;
            }
        }

        shutdown();

        std::unique_lock<std::shared_mutex> lock(mutex_);

        if (!needs_initialize_ && project_root_ == project_root && db_) {
            return true;
        }

        project_root_ = project_root;
        // gitignore 增量匹配缓存: 记录项目根, 祖先链解析到项目根为止
        // (不读取项目根之外的 .gitignore/.gitmodules 规则)
        gitignore_cache_.setRoot(project_root);
        // 索引数据库: {dataDir}/sqlite/codegraph/<折叠路径层级>/index.db
        // - 目录层级与项目路径层级一一对应 (深层路径折叠为 hash 段, 单段超长截断),
        //   长度受控不会超过系统路径限制 (Windows MAX_PATH=260 / Linux PATH_MAX)
        // - 支持路径前缀匹配复用 (findNearestExistingIndex):
        //   工作目录为已有索引项目的子目录时, 复用最近父级索引, 无需重新索引
        auto sqlite_base
            = fs::path(getCodeGraphSqliteDir(sqlite_dir_)) / kCodeGraphSqliteSubDirName;
        auto     reused     = findNearestExistingIndex(project_root, sqlite_dir_);
        fs::path index_path = reused ? *reused : getIndexDbPath(project_root, sqlite_dir_);
        if (reused) {
            XX_LOGI(
                "CodeGraphManager: reuse existing index db from parent path: {}",
                index_path.string()
            );
        }

        // 初始化失败记录日志并返回 false
        return agentxx::util::catchError<bool>(
            [&]() -> bool {
                if (!fs::exists(sqlite_base)) {
                    fs::create_directories(sqlite_base);
                }
                if (!fs::exists(index_path.parent_path())) {
                    fs::create_directories(index_path.parent_path());
                }
                // 多进程同时首次打开同一库时, 构造内的 WAL PRAGMA 可能短暂锁冲突,
                // 有限重试避免整个 codegraph 工具注册失败
                bool dbOpened = runWithRetry("open database", 3, [&]() {
                    db_ = std::make_unique<codegraph::Database>(index_path.string());
                });
                if (!dbOpened) {
                    return false;
                }
                db_->init_schema();
                traverser_        = std::make_unique<codegraph::GraphTraverser>(*db_);
                context_builder_  = std::make_unique<codegraph::ContextBuilder>(*db_, *traverser_);
                fts_search_       = std::make_unique<codegraph::FtsSearch>(*db_);
                needs_initialize_ = false;
                running_.store(true);
                return true;
            },
            [](std::string errmsg) -> bool {
                XX_LOGE("CodeGraphManager: initialize failed: {}", errmsg);
                return false;
            }
        );
    }

    bool indexDirectory(std::string_view path, bool incremental) {
        if (!db_) {
            return false;
        }

        // 每次索引前失效 gitignore 规则缓存: .gitignore/.gitmodules 可能在上次
        // 索引后被新增/修改, 否则收尾清理/增量判断仍沿用旧规则 (漏删/漏滤)
        gitignore_cache_.clear();

        // 索引生命周期标志: 查询侧据此附加"索引中, 结果可能不完整"提示;
        // RAII 保证所有 return 路径 (成功/中断/异常) 复位
        indexing_.store(true, std::memory_order_release);

        struct IndexingGuard {
            std::atomic<bool>& flag;

            ~IndexingGuard() {
                flag.store(false, std::memory_order_release);
            }
        } indexingGuard{indexing_};

        // 锁策略: 不再全程持有独占写锁 (否则索引期间查询被 writer 优先阻塞,
        // 表现为模型调用 codegraph tool 一直等待)。
        // - traverse_source_files / 文件解析 / is_changed 为锁外只读, 可与查询并发
        // - 仅"实际写库"的操作 (writeExtractionResult / 收尾清理 / 引用解析写 /
        //   rebuild_fts / wal checkpoint) 在持有 unique_lock 的短暂窗口内执行,
        //   查询 (shared_lock) 至多等待毫秒级, 大部分时间可读取已提交的部分数据
        // - 收尾阶段同样分阶段取锁: 消失文件判断 (磁盘 IO/规则匹配) 与引用
        //   解析计算在锁外或 shared_lock 下进行, 不阻塞查询
        // 流式遍历+索引: 目录遍历与文件索引同步进行 (不预先收集完整文件列表),
        // 大目录下索引进度随遍历逐渐增长, 内存中不缓存全部路径
        int  processed   = 0;
        auto last_notify = std::chrono::steady_clock::now();

        auto processFile = [&](std::string_view file_path) -> bool {
            // 停止信号: 中断遍历 (traverse 不再回调)
            if (!running_.load()) {
                return false;
            }

            if (incremental) {
                // 条目构建/变更判断失败时按"需重新索引"处理
                bool changed = agentxx::util::catchError<bool>(
                    [&]() -> bool {
                        fs::directory_entry entry(file_path);
                        return is_changed(*db_, entry, file_path);
                    },
                    [](std::string) -> bool {
                        return false;
                    }
                );
                if (!changed) {
                    // 增量跳过未变更文件: 推进进度计数 (processed = 已检查文件数),
                    // 使重启后断点续传时进度显示从上一次位置继续, 而非从 0 开始
                    ++processed;
                    return true;
                }
            }

            std::string lang = codegraph::detect_language(std::string{file_path});
            if (lang.empty()) {
                return true;
            }

            auto extractor = codegraph::create_extractor(std::string{lang});
            if (!extractor) {
                return true;
            }

            std::ifstream ifs(std::string{file_path});
            if (!ifs.is_open()) {
                XX_LOGW("CodeGraphManager: cannot open file {}", file_path);
                return true;
            }
            std::string source(
                (std::istreambuf_iterator<char>(ifs)),
                std::istreambuf_iterator<char>()
            );

            auto result = extractor->extract(std::string{file_path}, source);

            // 实际写库: 短暂 unique_lock (毫秒级), 查询 shared_lock 不会长期阻塞
            {
                std::unique_lock<std::shared_mutex> lock(mutex_);
                if (!db_) {
                    return false;
                }
                writeExtractionResult(file_path, lang, result);
            }

            ++processed;

            // 进度通知: 首个文件立即回调 (UI 尽快显示"已发现文件"), 之后按
            // 固定时间间隔节流 (kProgressNotifyInterval; total 在遍历结束前
            // 未知, 传 0 表示"文件总数未知, 索引进行中"):
            // UI 侧据此显示已发现的文件数, 随遍历逐渐增长
            auto now = std::chrono::steady_clock::now();
            if (progress_callback_
                && (processed == 1 || now - last_notify >= kProgressNotifyInterval)) {
                progress_callback_(processed, 0, file_path);
                last_notify = now;
            }

            // 每批文件提交后 WAL checkpoint: 已提交数据及时合并进主库文件,
            // 缩小"索引中途进程被强杀 -> 数据丢失"的窗口 (-wal 文件保留时
            // sqlite 会自动恢复, checkpoint 是防止 -wal 被外部清理的兜底)
            if (processed % kCheckpointFileBatch == 0) {
                std::unique_lock<std::shared_mutex> lock(mutex_);
                runWithRetry("WAL checkpoint", 3, [&]() {
                    db_->wal_checkpoint();
                });
            }
            return true;
        };

        // 边遍历边索引: 每发现一个源文件立即处理并通知进度
        traverse_source_files(
            path,
            ignore_path_regexes_,
            index_config_.useGitignore,
            processFile
        );

        // 遍历结束: 文件总数为已处理数 (含增量跳过的未变更文件)
        const int total = processed;

        if (total == 0) {
            XX_LOGW("CodeGraphManager: no source files found in {}", path);
            // 仍清理该根前缀下的残留记录 (忽略规则变更导致整根被过滤时,
            // 旧节点/引用若不删除会残留), 然后保持原语义提前返回
            // (cleanupRemovedFiles 内部自行管理锁)
            cleanupRemovedFiles(path);
            std::unique_lock<std::shared_mutex> lock(mutex_);
            if (db_) {
                runWithRetry("WAL checkpoint", 3, [&]() {
                    db_->wal_checkpoint();
                });
                invalidateCaches();
            }
            return true;
        }

        XX_LOGI("CodeGraphManager: indexed {} source files in {}", total, path);

        // 收尾: 各阶段尽量缩短独占锁持有时间, 避免查询 (codegraph tool 调用)
        // 在索引完成时被长时间阻塞:
        // - 消失文件清理: 内部三阶段 (快照 -> 锁外判断 -> 事务删除), 磁盘 IO
        //   与规则匹配不占锁
        // - 引用解析: 内部两阶段 (shared_lock 只读计算与查询并发 ->
        //   unique_lock 事务写)
        // - FTS 重建: 仅全量索引执行 (单条 insert/delete 走触发器同步,
        //   增量时 FTS 已一致, 重建冗余; 全量保留作兜底)
        // - WAL checkpoint: 短暂独占锁
        cleanupRemovedFiles(path);

        resolveReferences();

        {
            std::unique_lock<std::shared_mutex> lock(mutex_);
            if (!db_) {
                return false;
            }
            // FTS 重建失败仅告警, 不影响索引主流程; 多进程并发时锁竞争重试
            if (!incremental) {
                runWithRetry("FTS rebuild", 3, [&]() {
                    db_->rebuild_fts();
                });
            }

            // 索引结束 WAL checkpoint: 全部已提交数据落主库文件。进程被强杀
            // (无析构/无 sqlite close) 时主库仍是完整数据, 下次启动不会从头索引
            runWithRetry("WAL checkpoint", 3, [&]() {
                db_->wal_checkpoint();
            });

            // 索引数据已变更: 使全部查询缓存失效, 下次查询按新数据重算
            invalidateCaches();
        }

        // 索引进度完成信号 (约定 processed==total 且 total>0 表示索引结束):
        // 订阅方据此将状态置为 "完成" (indexing=false), 并作为最后一次进度回调
        if (progress_callback_) {
            progress_callback_(total, total, "");
        }

        return true;
    }

    bool updateIndex() {
        // 不在此处无锁读 db_ (避免与 initialize/shutdown 的数据竞争):
        // indexDirectory 内部持独占锁并检查 db_ 可用性
        // 配置了加载路径时按列表逐个增量索引; 否则按 autoLoadProjectRoot
        // 决定是否回退项目根目录 (false 且无加载路径: 无自动索引范围, 空操作)
        if (!index_config_.loadPaths.empty()) {
            bool all_ok = true;
            for (const auto& p : index_config_.loadPaths) {
                if (!indexDirectory(p, true)) {
                    all_ok = false;
                }
            }
            return all_ok;
        }
        if (!index_config_.autoLoadProjectRoot) {
            return true;
        }
        return indexDirectory(project_root_, true);
    }

    /// 引用解析: 把跨文件未解析引用解析为正式调用边
    /// - 两阶段执行, 尽量缩短独占锁持有时间:
    ///   1) 只读计算 (shared_lock, 与查询并发): 快照未解析引用 + 计算每条
    ///      的目标候选, 期间查询线程不被阻塞
    ///   2) 事务写 (短暂 unique_lock): 批量插入解析出的边 + 删除已处理引用
    bool resolveReferences() {
        struct ResolvedRef {
            int64_t source_id;
            int64_t target_id;
            int     line;
            int     col;
            int64_t ref_id;
        };
        std::vector<ResolvedRef> resolved;

        {
            // 阶段 1: 只读计算 (shared_lock, 查询可并发进入)
            std::shared_lock<std::shared_mutex> lock(mutex_);
            if (!db_) {
                return false;
            }
            auto unresolved = db_->get_unresolved_refs();
            if (unresolved.empty()) {
                return true;
            }
            resolved.reserve(unresolved.size());
            for (const auto& ref : unresolved) {
                if (!running_.load()) {
                    break;
                }
                auto source_node = db_->get_node(ref.source_node_id);
                if (!source_node.has_value()) {
                    continue;
                }
                auto candidates = db_->find_nodes_by_name(ref.ref_name, 10);
                if (candidates.empty()) {
                    continue;
                }
                int64_t target_id = candidates[0].id;
                if (candidates.size() > 1) {
                    codegraph::Node best       = candidates[0];
                    int             best_score = -1;
                    for (const auto& cand : candidates) {
                        int s = score_target(source_node.value(), cand);
                        if (s > best_score) {
                            best_score = s;
                            best       = cand;
                        }
                    }
                    target_id = best.id;
                }
                resolved.push_back(
                    ResolvedRef{source_node->id, target_id, ref.line, ref.col, ref.id}
                );
            }
        }

        if (resolved.empty()) {
            return true;
        }

        // 阶段 2: 事务写 (多进程并发写时锁竞争 (SQLITE_BUSY) 会导致整个
        // 引用解析批次丢失, 重试整个事务)
        std::unique_lock<std::shared_mutex> lock(mutex_);
        if (!db_) {
            return false;
        }
        return runTransactionWithRetry("resolveReferences", 3, db_.get(), [&]() {
            for (const auto& r : resolved) {
                if (!running_.load()) {
                    break;
                }
                codegraph::Edge edge;
                edge.source_id = r.source_id;
                edge.target_id = r.target_id;
                edge.kind      = codegraph::EdgeKind::Calls;
                edge.line      = r.line;
                edge.col       = r.col;
                db_->insert_edge(edge);
                db_->delete_unresolved_ref(r.ref_id);
            }
        });
    }

    CodeGraphSearchResult searchSymbols(std::string_view query, int limit) {
        // 缓存命中 (TTL 内同参数重复查询): 直接返回, 避免重复 FTS/排序
        std::string key = "search|" + std::string{query} + "|" + std::to_string(limit);
        if (auto hit = search_cache_.get(key)) {
            return *hit;
        }

        CodeGraphSearchResult result;
        {
            // 只读查询: 共享锁, 多个查询可并发执行 (索引写与查询读互斥)
            std::shared_lock<std::shared_mutex> lock(mutex_);
            if (!db_ || !fts_search_) {
                result.error = "CodeGraph not initialized";
                return result;
            }
            // 查询异常转为 result.error, 不向外抛出
            agentxx::util::catchError<bool>(
                [&]() -> bool {
                    result.nodes   = fts_search_->search(std::string{query}, limit);
                    result.success = true;
                    return true;
                },
                [&](std::string errmsg) -> bool {
                    result.error = std::move(errmsg);
                    return false;
                }
            );
        }
        // 仅缓存成功结果 (失败结果如 "未初始化" 无缓存价值)
        if (result.success) {
            search_cache_.put(std::move(key), result);
        }
        return result;
    }

    CodeGraphContextResult getSymbolContext(std::string_view symbol, int limit, int max_depth) {
        // 缓存命中 (TTL 内同参数重复查询): 直接返回, 避免重复图遍历
        std::string key = "ctx|" + std::string{symbol} + "|" + std::to_string(limit) + "|"
                          + std::to_string(max_depth);
        if (auto hit = context_cache_.get(key)) {
            return *hit;
        }

        CodeGraphContextResult result;
        {
            // 只读查询: 共享锁, 多个查询可并发执行
            std::shared_lock<std::shared_mutex> lock(mutex_);
            if (!context_builder_) {
                result.error = "CodeGraph not initialized";
                return result;
            }
            // 查询异常转为 result.error, 不向外抛出
            agentxx::util::catchError<bool>(
                [&]() -> bool {
                    result.context
                        = context_builder_->build_context(std::string{symbol}, limit, max_depth);
                    if (result.context.contains("error")) {
                        result.error   = result.context["error"].get<std::string>();
                        result.success = false;
                    } else {
                        result.success = true;
                    }
                    return true;
                },
                [&](std::string errmsg) -> bool {
                    result.error = std::move(errmsg);
                    return false;
                }
            );
        }
        if (result.success) {
            context_cache_.put(std::move(key), result);
        }
        return result;
    }

    CodeGraphImpactResult getCallers(std::string_view symbol, int max_depth) {
        // 缓存命中 (TTL 内同参数重复查询): 直接返回, 避免重复图遍历
        std::string key = "callers|" + std::string{symbol} + "|" + std::to_string(max_depth);
        if (auto hit = callers_cache_.get(key)) {
            return *hit;
        }

        CodeGraphImpactResult result;
        {
            // 只读查询: 共享锁, 多个查询可并发执行
            std::shared_lock<std::shared_mutex> lock(mutex_);
            if (!context_builder_) {
                result.error = "CodeGraph not initialized";
                return result;
            }
            // 查询异常转为 result.error, 不向外抛出
            agentxx::util::catchError<bool>(
                [&]() -> bool {
                    result.impact = context_builder_->get_callers(std::string{symbol}, max_depth);
                    if (result.impact.contains("error")) {
                        result.error   = result.impact["error"].get<std::string>();
                        result.success = false;
                    } else {
                        result.success = true;
                    }
                    return true;
                },
                [&](std::string errmsg) -> bool {
                    result.error = std::move(errmsg);
                    return false;
                }
            );
        }
        if (result.success) {
            callers_cache_.put(std::move(key), result);
        }
        return result;
    }

    CodeGraphImpactResult getCallees(std::string_view symbol, int max_depth) {
        // 缓存命中 (TTL 内同参数重复查询): 直接返回, 避免重复图遍历
        std::string key = "callees|" + std::string{symbol} + "|" + std::to_string(max_depth);
        if (auto hit = callees_cache_.get(key)) {
            return *hit;
        }

        CodeGraphImpactResult result;
        {
            // 只读查询: 共享锁, 多个查询可并发执行
            std::shared_lock<std::shared_mutex> lock(mutex_);
            if (!context_builder_) {
                result.error = "CodeGraph not initialized";
                return result;
            }
            // 查询异常转为 result.error, 不向外抛出
            agentxx::util::catchError<bool>(
                [&]() -> bool {
                    result.impact = context_builder_->get_callees(std::string{symbol}, max_depth);
                    if (result.impact.contains("error")) {
                        result.error   = result.impact["error"].get<std::string>();
                        result.success = false;
                    } else {
                        result.success = true;
                    }
                    return true;
                },
                [&](std::string errmsg) -> bool {
                    result.error = std::move(errmsg);
                    return false;
                }
            );
        }
        if (result.success) {
            callees_cache_.put(std::move(key), result);
        }
        return result;
    }

    CodeGraphImpactResult getImpact(std::string_view symbol, int max_depth) {
        // 缓存命中 (TTL 内同参数重复查询): 直接返回, 避免重复图遍历
        std::string key = "impact|" + std::string{symbol} + "|" + std::to_string(max_depth);
        if (auto hit = impact_cache_.get(key)) {
            return *hit;
        }

        CodeGraphImpactResult result;
        {
            // 只读查询: 共享锁, 多个查询可并发执行
            std::shared_lock<std::shared_mutex> lock(mutex_);
            if (!context_builder_) {
                result.error = "CodeGraph not initialized";
                return result;
            }
            // 查询异常转为 result.error, 不向外抛出
            agentxx::util::catchError<bool>(
                [&]() -> bool {
                    result.impact = context_builder_->get_impact(std::string{symbol}, max_depth);
                    if (result.impact.contains("error")) {
                        result.error   = result.impact["error"].get<std::string>();
                        result.success = false;
                    } else {
                        result.success = true;
                    }
                    return true;
                },
                [&](std::string errmsg) -> bool {
                    result.error = std::move(errmsg);
                    return false;
                }
            );
        }
        if (result.success) {
            impact_cache_.put(std::move(key), result);
        }
        return result;
    }

    CodeGraphPathResult findPath(std::string_view from, std::string_view to, int max_depth) {
        // 缓存命中 (TTL 内同参数重复查询): 直接返回, 避免重复图搜索
        std::string key
            = "path|" + std::string{from} + "|" + std::string{to} + "|" + std::to_string(max_depth);
        if (auto hit = path_cache_.get(key)) {
            return *hit;
        }

        CodeGraphPathResult result;
        {
            // 只读查询: 共享锁, 多个查询可并发执行
            std::shared_lock<std::shared_mutex> lock(mutex_);
            if (!db_ || !traverser_) {
                result.error = "CodeGraph not initialized";
                return result;
            }
            // 查询异常转为 result.error, 不向外抛出
            agentxx::util::catchError<bool>(
                [&]() -> bool {
                    auto from_nodes = db_->find_nodes_by_name(std::string{from}, 1);
                    auto to_nodes   = db_->find_nodes_by_name(std::string{to}, 1);
                    if (from_nodes.empty() || to_nodes.empty()) {
                        result.error = "Symbol not found";
                        return true;
                    }
                    auto path_ids
                        = traverser_->find_path(from_nodes[0].id, to_nodes[0].id, max_depth);
                    if (path_ids.empty()) {
                        result.error = "No path found";
                        return true;
                    }
                    auto nodes = db_->get_nodes_by_ids(path_ids);
                    std::unordered_map<int64_t, codegraph::Node> node_map;
                    for (auto& n : nodes) {
                        node_map[n.id] = n;
                    }
                    for (auto id : path_ids) {
                        auto it = node_map.find(id);
                        if (it != node_map.end()) {
                            result.path.push_back(it->second);
                        }
                    }
                    result.success = true;
                    return true;
                },
                [&](std::string errmsg) -> bool {
                    result.error = std::move(errmsg);
                    return false;
                }
            );
        }
        // 仅缓存成功结果 (失败结果如 "Symbol not found" 可能很快变化, 不缓存)
        if (result.success) {
            path_cache_.put(std::move(key), result);
        }
        return result;
    }

    CodeGraphStatusResult getStatus() {
        // 缓存命中 (TTL 内重复查询): 直接返回, 避免重复统计查询
        const std::string key = "status";
        if (auto hit = status_cache_.get(key)) {
            return *hit;
        }

        CodeGraphStatusResult result;
        {
            // 只读查询: 共享锁, 多个查询可并发执行
            std::shared_lock<std::shared_mutex> lock(mutex_);
            if (!db_ || !traverser_) {
                result.error = "CodeGraph not initialized";
                return result;
            }
            // 各统计项失败时带上下文记录错误并提前返回
            bool ok = agentxx::util::catchError<bool>(
                [&]() -> bool {
                    result.total_nodes = db_->count_nodes();
                    return true;
                },
                [&](std::string errmsg) -> bool {
                    result.error = std::string("count_nodes: ") + std::move(errmsg);
                    return false;
                }
            );
            if (!ok) {
                return result;
            }
            ok = agentxx::util::catchError<bool>(
                [&]() -> bool {
                    result.total_edges = db_->count_edges();
                    return true;
                },
                [&](std::string errmsg) -> bool {
                    result.error = std::string("count_edges: ") + std::move(errmsg);
                    return false;
                }
            );
            if (!ok) {
                return result;
            }
            ok = agentxx::util::catchError<bool>(
                [&]() -> bool {
                    result.total_files = static_cast<int64_t>(db_->get_all_files().size());
                    return true;
                },
                [&](std::string errmsg) -> bool {
                    result.error = std::string("get_all_files: ") + std::move(errmsg);
                    return false;
                }
            );
            if (!ok) {
                return result;
            }
            ok = agentxx::util::catchError<bool>(
                [&]() -> bool {
                    auto cycles          = traverser_->find_circular_dependencies();
                    result.circular_deps = static_cast<int>(cycles.size());
                    return true;
                },
                [&](std::string errmsg) -> bool {
                    result.error = std::string("find_circular_dependencies: ") + std::move(errmsg);
                    return false;
                }
            );
            if (!ok) {
                return result;
            }
            result.success = true;
        }
        // 仅缓存成功结果
        if (result.success) {
            status_cache_.put(key, result);
        }
        return result;
    }

    bool startFileWatcher(bool auto_reindex) {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        if (!db_ || file_watcher_running_) {
            return false;
        }

        // 监听根: 配置了加载路径时按列表监听; 否则按 autoLoadProjectRoot
        // 决定是否监听项目根目录 (false 且无加载路径: 无监听范围)
        std::vector<std::string> watch_roots;
        if (!index_config_.loadPaths.empty()) {
            watch_roots = index_config_.loadPaths;
        } else if (index_config_.autoLoadProjectRoot) {
            watch_roots.push_back(project_root_);
        }
        if (watch_roots.empty()) {
            XX_LOGW("CodeGraphManager: no watch roots (loadPaths empty and load_cwd disabled), "
                    "skip file watcher");
            return false;
        }

        // 启动文件监听失败记录日志并返回 false
        return agentxx::util::catchError<bool>(
            [&]() -> bool {
                file_watcher_ = codegraph::FileWatcher::create(project_root_, &running_);
                for (const auto& root : watch_roots) {
                    file_watcher_->add_watch_recursive(root);
                }

                file_watcher_->set_callback([this,
                                             auto_reindex](std::string_view path, uint32_t mask) {
                    if (auto_reindex
                        && (mask & (codegraph::FILE_EVENT_MODIFIED | codegraph::FILE_EVENT_CREATED)
                        )) {
                        // .gitignore/.gitmodules 变更: 失效规则缓存, 后续过滤
                        // 按最新规则重新解析 (不直接索引规则文件本身)
                        if (isGitRuleFile(path)) {
                            gitignore_cache_.clear();
                            return;
                        }
                        std::string lang = codegraph::detect_language(std::string{path});
                        // 与全量/增量索引一致的过滤: 内置过滤 + ignorePaths + gitignore
                        if (!lang.empty() && !this->isFileIgnored(std::string{path})) {
                            this->indexFile(path, lang);
                        }
                    }
                });

                file_watcher_running_ = true;
                running_.store(true);
                worker_thread_ = std::thread([this]() {
                    while (running_.load() && file_watcher_running_) {
                        // poll 异常仅记录日志, 线程继续轮询
                        agentxx::util::catchError<bool>(
                            [&]() -> bool {
                                file_watcher_->poll(1000);
                                return true;
                            },
                            [](std::string errmsg) -> bool {
                                XX_LOGE("CodeGraphManager: file watcher poll error: {}", errmsg);
                                return false;
                            }
                        );
                    }
                    file_watcher_->stop();
                });
                return true;
            },
            [](std::string errmsg) -> bool {
                XX_LOGE("CodeGraphManager: startFileWatcher error: {}", errmsg);
                return false;
            }
        );
    }

    void stopFileWatcher() {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        file_watcher_running_ = false;
        if (file_watcher_) {
            file_watcher_->stop();
        }
    }

    void setProgressCallback(IndexProgressCallback callback) {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        progress_callback_ = std::move(callback);
    }

    bool isRunning() const {
        return running_.load();
    }

    /// 索引是否进行中 (indexDirectory 生命周期内为 true):
    /// - 查询侧据此附加"索引中, 结果可能不完整"提示
    bool isIndexing() const {
        return indexing_.load(std::memory_order_acquire);
    }

    void writeExtractionResult(
        std::string_view             file_path,
        std::string_view             lang,
        codegraph::ExtractionResult& result
    ) {
        // 多进程并发写同一库时锁竞争 (SQLITE_BUSY) 会静默丢失该文件索引, 重试整个事务
        runTransactionWithRetry(fmt::format("write result for {}", file_path), 3, db_.get(), [&]() {
            db_->delete_edges_for_file_nodes(std::string{file_path});
            db_->delete_unresolved_refs_by_file(std::string{file_path});
            db_->delete_nodes_by_file(std::string{file_path});

            std::vector<int64_t> id_map;
            id_map.reserve(result.nodes.size());
            for (auto& node : result.nodes) {
                if (node.kind == codegraph::NodeKind::File) {
                    node.file_path = file_path;
                }
                int64_t id = db_->insert_node(node);
                id_map.push_back(id);
            }

            codegraph::FileRecord fr;
            fr.path     = file_path;
            fr.language = lang;
            // 无法读取文件元信息时使用默认值 (mtime=0), 不中断写入
            agentxx::util::catchError<bool>(
                [&]() -> bool {
                    auto ftime = fs::last_write_time(fs::path(file_path));
                    fr.mtime
                        = std::chrono::duration_cast<std::chrono::seconds>(ftime.time_since_epoch())
                              .count();
                    fr.size = fs::file_size(fs::path(file_path));
                    return true;
                },
                [](std::string) -> bool {
                    return false;
                }
            );
            db_->insert_file(fr);

            for (auto ref : result.unresolved) {
                int original_index = static_cast<int>(-ref.source_node_id) - 1;
                if (original_index >= 0 && original_index < static_cast<int>(id_map.size())) {
                    ref.source_node_id = id_map[original_index];
                }
                db_->insert_unresolved_ref(ref);
            }
        });
    }

    /// 单文件是否应被过滤 (内置过滤 + ignorePaths 正则 + gitignore 缓存)
    /// - 供文件监听增量索引与收尾清理使用; 全量/增量索引走
    ///   traverse_source_files 的遍历过滤 (gitignore 按层级加载),
    ///   两者过滤规则保持一致
    bool isFileIgnored(std::string_view file_path) {
        std::string s{file_path};
        if (should_skip(s)) {
            return true;
        }
        for (const auto& re : ignore_path_regexes_) {
            if (std::regex_match(s, re)) {
                return true;
            }
        }
        if (index_config_.useGitignore && gitignore_cache_.isIgnored(fs::path(s))) {
            return true;
        }
        return false;
    }

    /// 删除本次索引根前缀下、已消失的文件的残留记录
    /// - 文件被删除或命中忽略规则 (ignorePaths/.gitignore/.gitmodules) 后不再
    ///   被遍历收集, 旧节点/引用/文件记录若不删除会残留 (查询仍可搜到);
    ///   由 indexDirectory 收尾调用
    /// - 流式遍历不再保留收集文件集合 (避免大目录内存缓存), 改为对数据库记录
    ///   逐个判断: 文件仍存在且未命中过滤规则则保留, 否则删除 (语义与
    ///   "不在收集列表中的删除" 等价)
    /// - 仅删除属于 root 前缀的文件, 不动前缀复用索引库中其他路径的数据
    /// - 三阶段执行, 尽量缩短独占锁持有时间:
    ///   1) 锁内快照全部文件记录路径 (一次只读查询)
    ///   2) 锁外判断: fs::exists (磁盘 IO) 与 isFileIgnored (正则/gitignore
    ///      匹配) 全部移出独占锁, 期间查询可并发执行
    ///   3) 锁内单事务批量删除, 提交一次
    void cleanupRemovedFiles(std::string_view root) {
        std::string prefix = normalizePathStr(root);
        if (!prefix.empty() && prefix.back() != '/') {
            prefix.push_back('/');
        }

        // 阶段 1: 锁内快照文件记录 (短暂独占锁, 仅拷贝路径字符串)
        std::vector<std::string> all_paths;
        {
            std::unique_lock<std::shared_mutex> lock(mutex_);
            if (!db_) {
                return;
            }
            auto files = db_->get_all_files();
            all_paths.reserve(files.size());
            for (const auto& fr : files) {
                all_paths.push_back(fr.path);
            }
        }

        // 阶段 2: 锁外判断哪些需要删除 (磁盘 IO / 正则匹配不占锁)
        // isFileIgnored 内部访问 gitignore_cache_ (自带互斥) 与只读的
        // ignore_path_regexes_, 锁外调用安全
        std::vector<std::string> to_delete;
        to_delete.reserve(all_paths.size() / 4);
        for (const auto& path : all_paths) {
            std::string fp = normalizePathStr(path);
            // 不属于本次索引根前缀 (含前缀复用库中其他路径): 跳过
            if (fp.rfind(prefix, 0) != 0) {
                continue;
            }
            // 文件仍存在且未命中过滤规则: 保留 (增量索引跳过的未变更文件在此保留)
            if (fs::exists(fs::path(path)) && !isFileIgnored(path)) {
                continue;
            }
            to_delete.push_back(path);
        }
        if (to_delete.empty()) {
            return;
        }

        // 阶段 3: 锁内单事务批量删除 (顺序与 writeExtractionResult 一致:
        // 先删边/引用 (外键), 再删节点/记录; 单文件失败仅记录日志不中断,
        // 下次增量索引可再清理)
        std::unique_lock<std::shared_mutex> lock(mutex_);
        if (!db_) {
            return;
        }
        runTransactionWithRetry("cleanup removed files", 3, db_.get(), [&]() {
            for (const auto& path : to_delete) {
                agentxx::util::catchError<bool>(
                    [&]() -> bool {
                        db_->delete_edges_for_file_nodes(path);
                        db_->delete_unresolved_refs_by_file(path);
                        db_->delete_nodes_by_file(path);
                        db_->delete_file(path);
                        return true;
                    },
                    [&](std::string errmsg) -> bool {
                        XX_LOGW(
                            "CodeGraphManager: cleanup removed file {} failed: {}",
                            path,
                            errmsg
                        );
                        return false;
                    }
                );
                XX_LOGI("CodeGraphManager: cleanup removed file record: {}", path);
            }
        });
    }

    void indexFile(std::string_view file_path, std::string_view lang) {
        if (!db_) {
            return;
        }

        auto extractor = codegraph::create_extractor(std::string{lang});
        if (!extractor) {
            return;
        }

        std::ifstream ifs(std::string{file_path});
        if (!ifs.is_open()) {
            return;
        }
        std::string source((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());

        // 锁策略同 indexDirectory: 文件解析锁外 (可与查询并发),
        // 仅实际写库时短暂持独占锁
        auto result = extractor->extract(std::string{file_path}, source);
        {
            std::unique_lock<std::shared_mutex> lock(mutex_);
            if (!db_) {
                return;
            }
            writeExtractionResult(file_path, lang, result);
        }

        // 单文件增量索引完成: 使查询缓存失效 (该文件相关查询需按新数据重算)
        invalidateCaches();
    }

    /// 公开 resolveReferences API 入口: resolveReferences 内部自行管理锁
    /// (shared_lock 只读计算 + unique_lock 事务写), 此处不再额外取锁
    bool resolveReferencesLocked() {
        return resolveReferences();
    }

    /// 索引/文件/引用数据变更后使全部查询缓存失效 (LruCache generation+1)
    /// - 仅由写路径 (indexDirectory / indexFile / resolveReferences) 调用,
    ///   调用方须已持有独占锁; LruCache 内部自带互斥, 无锁嵌套问题
    void invalidateCaches() {
        search_cache_.invalidate();
        context_cache_.invalidate();
        callers_cache_.invalidate();
        callees_cache_.invalidate();
        impact_cache_.invalidate();
        path_cache_.invalidate();
        status_cache_.invalidate();
    }

    /// 每批处理的文件数, 达到后执行一次 WAL checkpoint
    /// - 缩小"索引中途进程被强杀 -> 已提交数据仅存于 -wal"的丢失窗口
    static constexpr int kCheckpointFileBatch = 200;

    /// 索引进度通知间隔: 每间隔该时长推送一次进度回调 (遍历阶段文件总数未知)
    /// - 流式遍历时每文件一次回调的 post/字符串拷贝开销对大目录 (数万文件)
    ///   可感知, 定时通知既保持 UI 进度平滑增长又控制开销;
    ///   首个文件仍立即通知 (UI 尽快显示"已发现文件"), 完成信号不受间隔约束
    static constexpr auto kProgressNotifyInterval = std::chrono::seconds{5};

private:

    // ------------------------------------------------------------------
    // 查询/索引并发控制
    //
    // shared_mutex 读写分离:
    // - 查询 (searchSymbols 等只读操作): shared_lock, 多个查询可并发执行
    // - 索引/初始化等写操作: unique_lock, 独占执行
    // - sqlite3 以 serialized 模式编译 (agentxx 构建开启
    //   sqlite3_ENABLE_THREADSAFE), 多线程共享同一 Database 连接并发执行
    //   只读 SQL (每次 prepare->step->finalize, 不共享 prepared stmt) 安全;
    //   写操作仍须串行 (WAL 写不阻塞读其他连接, 但本架构单连接, 由锁协调)
    std::shared_mutex mutex_;

    // ------------------------------------------------------------------
    // 查询结果缓存 (LruCache: LRU 淘汰 + TTL 过期 + generation 主动失效)
    //
    // LLM 循环中高频重复查询 (同一符号反复查 context/callers 等), 无缓存时
    // 每次实时 FTS/图遍历, 浪费计算并拖慢响应; 命中缓存直接返回 (TTL 30s),
    // 索引数据变更后 invalidate() 使旧结果失效, 下次查询按新数据重算
    codegraph::LruCache<std::string, CodeGraphSearchResult>  search_cache_{512, 30};
    codegraph::LruCache<std::string, CodeGraphContextResult> context_cache_{512, 30};
    codegraph::LruCache<std::string, CodeGraphImpactResult>  callers_cache_{512, 30};
    codegraph::LruCache<std::string, CodeGraphImpactResult>  callees_cache_{512, 30};
    codegraph::LruCache<std::string, CodeGraphImpactResult>  impact_cache_{512, 30};
    codegraph::LruCache<std::string, CodeGraphPathResult>    path_cache_{256, 30};
    codegraph::LruCache<std::string, CodeGraphStatusResult>  status_cache_{16, 30};

    std::string                                project_root_;
    std::unique_ptr<codegraph::Database>       db_;
    std::unique_ptr<codegraph::GraphTraverser> traverser_;
    std::unique_ptr<codegraph::ContextBuilder> context_builder_;
    std::unique_ptr<codegraph::FtsSearch>      fts_search_;
    std::unique_ptr<codegraph::FileWatcher>    file_watcher_;

    std::thread             worker_thread_;
    std::condition_variable cv_;
    std::atomic<bool>       running_;
    /// 索引是否进行中 (indexDirectory 生命周期内为 true; 查询侧提示用)
    std::atomic<bool> indexing_{false};
    std::atomic<bool> file_watcher_running_{false};
    bool              needs_initialize_;
    /// sqlite 数据目录 (为空使用默认 {dataDir}/sqlite/, 见 getCodeGraphSqliteDir)
    std::string sqlite_dir_;

    // ------------------------------------------------------------------
    // 索引过滤配置 (构造时传入; 见 CodeGraphIndexConfig)
    CodeGraphIndexConfig index_config_;
    /// ignorePaths 编译后的正则列表 (含通配符: glob 语义; 无通配符: 目录前缀)
    std::vector<std::regex> ignore_path_regexes_;
    /// gitignore 匹配缓存 (供文件监听单文件增量索引; 与索引线程并发安全)
    GitIgnoreCache gitignore_cache_;

    IndexProgressCallback progress_callback_;
};

/// @param sqliteDir sqlite 数据目录; 为空使用默认 {dataDir}/sqlite/
///        (dataDir 为空时 ~/.agentxx/sqlite/, 取不到主目录时回退系统临时目录)
/// @param config 索引过滤配置 (加载路径/忽略路径/gitignore 开关)
CodeGraphManager::CodeGraphManager(std::string sqliteDir, CodeGraphIndexConfig config) :
    impl_(std::make_unique<Impl>(std::move(sqliteDir), std::move(config))) {}

CodeGraphManager::~CodeGraphManager() {
    impl_->shutdown();
}

bool CodeGraphManager::initialize(std::string_view project_root) {
    return impl_->initialize(project_root);
}

void CodeGraphManager::shutdown() {
    impl_->shutdown();
}

bool CodeGraphManager::isRunning() const {
    return impl_->isRunning();
}

bool CodeGraphManager::isIndexing() const {
    return impl_->isIndexing();
}

bool CodeGraphManager::indexDirectory(std::string_view path, bool incremental) {
    return impl_->indexDirectory(path, incremental);
}

bool CodeGraphManager::updateIndex() {
    return impl_->updateIndex();
}

bool CodeGraphManager::resolveReferences() {
    return impl_->resolveReferencesLocked();
}

CodeGraphSearchResult CodeGraphManager::searchSymbols(std::string_view query, int limit) {
    return impl_->searchSymbols(query, limit);
}

CodeGraphContextResult
    CodeGraphManager::getSymbolContext(std::string_view symbol, int limit, int max_depth) {
    return impl_->getSymbolContext(symbol, limit, max_depth);
}

CodeGraphImpactResult CodeGraphManager::getCallers(std::string_view symbol, int max_depth) {
    return impl_->getCallers(symbol, max_depth);
}

CodeGraphImpactResult CodeGraphManager::getCallees(std::string_view symbol, int max_depth) {
    return impl_->getCallees(symbol, max_depth);
}

CodeGraphImpactResult CodeGraphManager::getImpact(std::string_view symbol, int max_depth) {
    return impl_->getImpact(symbol, max_depth);
}

CodeGraphPathResult
    CodeGraphManager::findPath(std::string_view from, std::string_view to, int max_depth) {
    return impl_->findPath(from, to, max_depth);
}

CodeGraphStatusResult CodeGraphManager::getStatus() {
    return impl_->getStatus();
}

bool CodeGraphManager::startFileWatcher(bool auto_reindex) {
    return impl_->startFileWatcher(auto_reindex);
}

void CodeGraphManager::stopFileWatcher() {
    impl_->stopFileWatcher();
}

void CodeGraphManager::setProgressCallback(IndexProgressCallback callback) {
    impl_->setProgressCallback(std::move(callback));
}

} // namespace expand
} // namespace agentxx

#endif