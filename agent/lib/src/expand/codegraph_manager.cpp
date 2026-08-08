#include "agentxx/expand/codegraph_manager.h"
#include "agentxx/agent/config_static.h"
#include "agentxx/util/exception.h"
#include "agentxx/util/log.h"
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
#include <sstream>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#if AGENTXX_ENABLE_CODEGRAPH
#include "codegraph/context/context_builder.h"
#include "codegraph/core/types.h"
#include "codegraph/db/database.h"
#include "codegraph/extraction/extractor.h"
#include "codegraph/graph/traverser.h"
#include "codegraph/search/fts_search.h"
#include "codegraph/sync/file_watcher.h"

namespace agentxx {
namespace expand {

namespace fs = std::filesystem;

/// CodeGraph sqlite 数据库存放目录名: ~/.agentxx/sqlite/
static constexpr std::string_view kCodeGraphSqliteDirName = "sqlite";
/// CodeGraph 索引数据库子目录: ~/.agentxx/sqlite/codegraph/
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

/// 获取用户主目录 (Unix: $HOME, Windows: %USERPROFILE%)
/// - 未设置时返回空字符串
static std::string getUserHomeDir() {
#if XX_IS_WIN_D
    const char* home = std::getenv("USERPROFILE");
#else
    const char* home = std::getenv("HOME");
#endif
    if (!home || !*home) {
        return "";
    }
    return std::string{home};
}

/// CodeGraph sqlite 数据库目录: ~/.agentxx/sqlite/
/// - 取不到用户主目录时回退到系统临时目录, 保证功能可用
static std::string getCodeGraphSqliteDir() {
    auto home = getUserHomeDir();
    if (!home.empty()) {
        return (fs::path(home) / agentxx::agent::AgentConfigStatic::agentxxDataDirPath
                / kCodeGraphSqliteDirName)
            .string();
    }
    return (fs::temp_directory_path() / agentxx::agent::AgentConfigStatic::agentxxDataDirPath
            / kCodeGraphSqliteDirName)
        .string();
}

/// 路径段清洗: 替换文件系统非法字符为 `_`
/// - Windows 非法字符: < > : " / \ | ? * 及 ASCII 0-31
static std::string sanitizeSegment(std::string_view seg) {
    std::string out;
    out.reserve(seg.size());
    for (char c : seg) {
        if (static_cast<unsigned char>(c) < 0x20 || c == '<' || c == '>' || c == ':'
            || c == '"' || c == '/' || c == '\\' || c == '|' || c == '?' || c == '*') {
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
        "CON", "PRN", "AUX", "NUL",
        "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
        "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9",
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
    bool ok = agentxx::util::catchError<bool>(
        [&]() -> bool {
            abs = fs::weakly_canonical(fs::path(project_root));
            return true;
        },
        [](std::string) -> bool { return false; }
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

/// 项目根目录对应的索引数据库路径: ~/.agentxx/sqlite/codegraph/<折叠路径层级>/index.db
static fs::path getIndexDbPath(std::string_view project_root) {
    fs::path dir = fs::path(getCodeGraphSqliteDir()) / kCodeGraphSqliteSubDirName;
    for (const auto& seg : foldSegments(projectRootToSegments(project_root))) {
        dir /= seg;
    }
    return dir / kCodeGraphIndexDbName;
}

/// 路径前缀匹配: 沿项目路径逐级向上查找最近已存在的索引数据库
/// - 例如工作目录为 /home/user/proj/sub 时, 若 /home/user/proj 已建索引则复用
/// - 每个前缀独立应用折叠规则 (确定性), 折叠后路径一致即视为同一前缀
/// - 未找到任何父级索引时返回 nullopt (将在自身路径下新建)
static std::optional<fs::path> findNearestExistingIndex(std::string_view project_root) {
    auto segs = projectRootToSegments(project_root);
    if (segs.empty()) {
        return std::nullopt;
    }
    fs::path base = fs::path(getCodeGraphSqliteDir()) / kCodeGraphSqliteSubDirName;
    // 从最深 (最近) 前缀开始逐级缩短检查
    for (size_t n = segs.size(); n > 0; --n) {
        std::vector<std::string> prefix(segs.begin(), segs.begin() + static_cast<std::ptrdiff_t>(n));
        fs::path                 dir = base;
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
template <typename F>
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
template <typename F>
static bool runTransactionWithRetry(std::string_view what, int attempts, codegraph::Database* db, F&& fn) {
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
                        [](std::string) -> bool { return false; }
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

static std::vector<std::string> collect_source_files(std::string_view root_path) {
    std::vector<std::string> files;
    // 遍历失败记录日志, 返回已收集到的部分文件
    agentxx::util::catchError<bool>(
        [&]() -> bool {
            for (const auto& entry : fs::recursive_directory_iterator(
                     root_path,
                     fs::directory_options::skip_permission_denied
                 )) {
                if (!entry.is_regular_file()) {
                    continue;
                }
                std::string path = entry.path().generic_string();
                if (should_skip(path)) {
                    continue;
                }
                std::string lang = codegraph::detect_language(path);
                if (lang.empty()) {
                    continue;
                }
                files.push_back(path);
            }
            return true;
        },
        [](std::string errmsg) -> bool {
            XX_LOGE("CodeGraphManager: collect_source_files error: {}", errmsg);
            return false;
        }
    );
    return files;
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
            auto mtime
                = std::chrono::duration_cast<std::chrono::seconds>(ftime.time_since_epoch()).count();
            return existing->mtime != mtime
                   || existing->size != static_cast<int64_t>(fs::file_size(entry));
        },
        [](std::string) -> bool { return true; }
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

    Impl() :
        running_(false),
        needs_initialize_(true) {}

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
        needs_initialize_ = true;
    }

    bool initialize(std::string_view project_root) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!needs_initialize_ && project_root_ == project_root && db_) {
                return true;
            }
        }

        shutdown();

        std::lock_guard<std::mutex> lock(mutex_);

        if (!needs_initialize_ && project_root_ == project_root && db_) {
            return true;
        }

        project_root_ = project_root;
        // 索引数据库: ~/.agentxx/sqlite/codegraph/<折叠路径层级>/index.db
        // - 目录层级与项目路径层级一一对应 (深层路径折叠为 hash 段, 单段超长截断),
        //   长度受控不会超过系统路径限制 (Windows MAX_PATH=260 / Linux PATH_MAX)
        // - 支持路径前缀匹配复用 (findNearestExistingIndex):
        //   工作目录为已有索引项目的子目录时, 复用最近父级索引, 无需重新索引
        auto     sqlite_base = fs::path(getCodeGraphSqliteDir()) / kCodeGraphSqliteSubDirName;
        auto     reused      = findNearestExistingIndex(project_root);
        fs::path index_path  = reused ? *reused : getIndexDbPath(project_root);
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
        std::lock_guard<std::mutex> lock(mutex_);
        if (!db_) {
            return false;
        }

        auto files = collect_source_files(path);
        if (files.empty()) {
            XX_LOGW("CodeGraphManager: no source files found in {}", path);
            return true;
        }

        XX_LOGI("CodeGraphManager: found {} source files to index", files.size());

        int processed = 0;
        int total     = static_cast<int>(files.size());

        for (const auto& file_path : files) {
            if (!running_.load()) {
                break;
            }

            XX_LOGI("CodeGraphManager: processing file [{}]: {}", processed, file_path);

            if (incremental) {
                // 条目构建/变更判断失败时按"需重新索引"处理
                bool changed = agentxx::util::catchError<bool>(
                    [&]() -> bool {
                        fs::directory_entry entry(file_path);
                        return is_changed(*db_, entry, file_path);
                    },
                    [](std::string) -> bool { return false; }
                );
                if (!changed) {
                    continue;
                }
            }

            if (progress_callback_) {
                progress_callback_(processed, total, file_path);
            }

            std::string lang = codegraph::detect_language(file_path);
            if (lang.empty()) {
                XX_LOGW("CodeGraphManager: no language detected for {}", file_path);
                continue;
            }

            auto extractor = codegraph::create_extractor(std::string{lang});
            if (!extractor) {
                XX_LOGW("CodeGraphManager: no extractor for lang={} file={}", lang, file_path);
                continue;
            }

            std::ifstream ifs(std::string{file_path});
            if (!ifs.is_open()) {
                XX_LOGW("CodeGraphManager: cannot open file {}", file_path);
                continue;
            }
            std::string source(
                (std::istreambuf_iterator<char>(ifs)),
                std::istreambuf_iterator<char>()
            );

            auto result = extractor->extract(std::string{file_path}, source);
            XX_LOGI(
                "CodeGraphManager: extracted {} nodes, {} unresolved refs from {}",
                result.nodes.size(),
                result.unresolved.size(),
                file_path
            );

            writeExtractionResult(file_path, lang, result);

            processed++;
        }

        resolveReferences();

        // FTS 重建失败仅告警, 不影响索引主流程; 多进程并发时锁竞争重试
        runWithRetry("FTS rebuild", 3, [&]() { db_->rebuild_fts(); });

        return true;
    }

    bool updateIndex() {
        if (!db_) {
            return false;
        }
        return indexDirectory(project_root_, true);
    }

    bool resolveReferences() {
        if (!db_) {
            return false;
        }

        auto unresolved = db_->get_unresolved_refs();
        if (unresolved.empty()) {
            return true;
        }

        // 多进程并发写时锁竞争 (SQLITE_BUSY) 会导致整个引用解析批次丢失, 重试整个事务
        return runTransactionWithRetry(
            "resolveReferences",
            3,
            db_.get(),
            [&]() {
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

                    if (candidates.size() == 1) {
                        codegraph::Edge edge;
                        edge.source_id = source_node->id;
                        edge.target_id = candidates[0].id;
                        edge.kind      = codegraph::EdgeKind::Calls;
                        edge.line      = ref.line;
                        edge.col       = ref.col;
                        db_->insert_edge(edge);
                    } else {
                        codegraph::Node best       = candidates[0];
                        int             best_score = -1;
                        for (const auto& cand : candidates) {
                            int s = score_target(source_node.value(), cand);
                            if (s > best_score) {
                                best_score = s;
                                best       = cand;
                            }
                        }
                        codegraph::Edge edge;
                        edge.source_id = source_node->id;
                        edge.target_id = best.id;
                        edge.kind      = codegraph::EdgeKind::Calls;
                        edge.line      = ref.line;
                        edge.col       = ref.col;
                        db_->insert_edge(edge);
                    }

                    db_->delete_unresolved_ref(ref.id);
                }
            }
        );
    }

    CodeGraphSearchResult searchSymbols(std::string_view query, int limit) {
        CodeGraphSearchResult       result;
        std::lock_guard<std::mutex> lock(mutex_);
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
        return result;
    }

    CodeGraphContextResult getSymbolContext(std::string_view symbol, int limit, int max_depth) {
        CodeGraphContextResult      result;
        std::lock_guard<std::mutex> lock(mutex_);
        if (!context_builder_) {
            result.error = "CodeGraph not initialized";
            return result;
        }
        // 查询异常转为 result.error, 不向外抛出
        agentxx::util::catchError<bool>(
            [&]() -> bool {
                result.context = context_builder_->build_context(std::string{symbol}, limit, max_depth);
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
        return result;
    }

    CodeGraphImpactResult getCallers(std::string_view symbol, int max_depth) {
        CodeGraphImpactResult       result;
        std::lock_guard<std::mutex> lock(mutex_);
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
        return result;
    }

    CodeGraphImpactResult getCallees(std::string_view symbol, int max_depth) {
        CodeGraphImpactResult       result;
        std::lock_guard<std::mutex> lock(mutex_);
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
        return result;
    }

    CodeGraphImpactResult getImpact(std::string_view symbol, int max_depth) {
        CodeGraphImpactResult       result;
        std::lock_guard<std::mutex> lock(mutex_);
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
        return result;
    }

    CodeGraphPathResult findPath(std::string_view from, std::string_view to, int max_depth) {
        CodeGraphPathResult         result;
        std::lock_guard<std::mutex> lock(mutex_);
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
                auto path_ids = traverser_->find_path(from_nodes[0].id, to_nodes[0].id, max_depth);
                if (path_ids.empty()) {
                    result.error = "No path found";
                    return true;
                }
                auto                                         nodes = db_->get_nodes_by_ids(path_ids);
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
        return result;
    }

    CodeGraphStatusResult getStatus() {
        CodeGraphStatusResult       result;
        std::lock_guard<std::mutex> lock(mutex_);
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
        return result;
    }

    bool startFileWatcher(bool auto_reindex) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!db_ || file_watcher_running_) {
            return false;
        }

        // 启动文件监听失败记录日志并返回 false
        return agentxx::util::catchError<bool>(
            [&]() -> bool {
                file_watcher_ = codegraph::FileWatcher::create(project_root_, &running_);
                file_watcher_->add_watch_recursive(project_root_);

                file_watcher_->set_callback([this, auto_reindex](std::string_view path, uint32_t mask) {
                    if (auto_reindex
                        && (mask & (codegraph::FILE_EVENT_MODIFIED | codegraph::FILE_EVENT_CREATED))) {
                        std::string lang = codegraph::detect_language(std::string{path});
                        if (!lang.empty() && !should_skip(path)) {
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
        file_watcher_running_ = false;
        if (file_watcher_) {
            file_watcher_->stop();
        }
    }

    void setProgressCallback(IndexProgressCallback callback) {
        progress_callback_ = std::move(callback);
    }

    bool isRunning() const {
        return running_.load();
    }

    void writeExtractionResult(
        std::string_view             file_path,
        std::string_view             lang,
        codegraph::ExtractionResult& result
    ) {
        // 多进程并发写同一库时锁竞争 (SQLITE_BUSY) 会静默丢失该文件索引, 重试整个事务
        runTransactionWithRetry(
            fmt::format("write result for {}", file_path),
            3,
            db_.get(),
            [&]() {
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
                    [](std::string) -> bool { return false; }
                );
                db_->insert_file(fr);

                for (auto ref : result.unresolved) {
                    int original_index = static_cast<int>(-ref.source_node_id) - 1;
                    if (original_index >= 0 && original_index < static_cast<int>(id_map.size())) {
                        ref.source_node_id = id_map[original_index];
                    }
                    db_->insert_unresolved_ref(ref);
                }
            }
        );
    }

    void indexFile(std::string_view file_path, std::string_view lang) {
        std::lock_guard<std::mutex> lock(mutex_);
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

        auto result = extractor->extract(std::string{file_path}, source);
        writeExtractionResult(file_path, lang, result);
    }

    bool resolveReferencesLocked() {
        std::lock_guard<std::mutex> lock(mutex_);
        return resolveReferences();
    }

private:

    std::string                                project_root_;
    std::unique_ptr<codegraph::Database>       db_;
    std::unique_ptr<codegraph::GraphTraverser> traverser_;
    std::unique_ptr<codegraph::ContextBuilder> context_builder_;
    std::unique_ptr<codegraph::FtsSearch>      fts_search_;
    std::unique_ptr<codegraph::FileWatcher>    file_watcher_;

    std::mutex              mutex_;
    std::thread             worker_thread_;
    std::condition_variable cv_;
    std::atomic<bool>       running_;
    std::atomic<bool>       file_watcher_running_{false};
    bool                    needs_initialize_;

    IndexProgressCallback progress_callback_;
};

CodeGraphManager::CodeGraphManager() :
    impl_(std::make_unique<Impl>()) {}

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