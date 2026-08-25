#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#if AGENTXX_ENABLE_PLUGIN_CODEGRAPH
#include "codegraph/core/json.hpp"
#include "codegraph/core/types.h"

namespace agentxx_codegraph_plugin {

struct CodeGraphSearchResult {
    std::vector<codegraph::Node> nodes;
    std::string                  error;
    bool                         success = false;
};

struct CodeGraphContextResult {
    codegraph::Json context;
    std::string     error;
    bool            success = false;
};

struct CodeGraphStatusResult {
    int64_t     total_nodes   = 0;
    int64_t     total_edges   = 0;
    int64_t     total_files   = 0;
    int         circular_deps = 0;
    std::string error;
    bool        success = false;
};

struct CodeGraphImpactResult {
    codegraph::Json impact;
    std::string     error;
    bool            success = false;
};

struct CodeGraphPathResult {
    std::vector<codegraph::Node> path;
    std::string                  error;
    bool                         success = false;
};

/// CodeGraph 索引过滤配置 (构造时传入, 默认值即全默认行为)
struct CodeGraphIndexConfig {
    /// 加载(索引)路径列表 (绝对路径; 调用方负责按工作目录解析相对路径)
    /// - 非空时 updateIndex/启动预热/文件监听按此列表执行 (可多个目录);
    ///   为空时按 autoLoadProjectRoot 决定是否回退项目根目录
    std::vector<std::string> loadPaths;
    /// 忽略路径列表 (绝对路径, 支持 * 通配符; 命中即跳过)
    /// - 对全部加载路径及 agentxx_codegraph_index 手动索引均生效
    std::vector<std::string> ignorePaths;
    /// 是否启用 git 相关忽略 (默认 true):
    /// - 逐层读取各级目录的 .gitignore 规则与 .gitmodules 子模块目录
    /// - 内置忽略 `.git` 元数据目录 (任意层级)
    bool useGitignore = true;
    /// loadPaths 为空时是否自动回退项目根目录 (yaml `codegraph.load_cwd`, 默认 true)
    /// - false 且 loadPaths 为空: 无自动索引范围 (updateIndex 空操作,
    ///   文件监听不启动), 仅 agentxx_codegraph_index 手动索引可用
    bool autoLoadProjectRoot = true;
};

class CodeGraphManager {
public:

    /// @param sqliteDir sqlite 数据目录; 为空使用默认 {dataDir}/sqlite/
    ///        (dataDir 为空时 ~/.agentxx/sqlite/, 取不到主目录时回退系统临时目录)
    /// @param config    索引过滤配置 (加载路径/忽略路径/gitignore 开关)
    explicit CodeGraphManager(std::string sqliteDir = "", CodeGraphIndexConfig config = {});
    ~CodeGraphManager();

    CodeGraphManager(const CodeGraphManager&)            = delete;
    CodeGraphManager& operator=(const CodeGraphManager&) = delete;

    bool initialize(std::string_view project_root);
    void shutdown();

    bool isRunning() const;

    /// 索引是否进行中 (indexDirectory 生命周期内为 true)
    /// - 查询侧据此附加"索引中, 结果可能不完整"提示
    bool isIndexing() const;

    bool indexDirectory(std::string_view path, bool incremental = true);
    bool updateIndex();
    bool resolveReferences();

    CodeGraphSearchResult searchSymbols(std::string_view query, int limit = 20);
    CodeGraphContextResult
        getSymbolContext(std::string_view symbol, int limit = 10, int max_depth = 3);
    CodeGraphImpactResult getCallers(std::string_view symbol, int max_depth = 3);
    CodeGraphImpactResult getCallees(std::string_view symbol, int max_depth = 3);
    CodeGraphImpactResult getImpact(std::string_view symbol, int max_depth = 5);
    CodeGraphPathResult   findPath(std::string_view from, std::string_view to, int max_depth = 10);
    CodeGraphStatusResult getStatus();

    bool startFileWatcher(bool auto_reindex = true);
    void stopFileWatcher();

    /// 索引进度回调 (由索引线程触发, 调用方须自行跨线程转发)
    /// - 流式遍历阶段: 每 kProgressNotifyBatch 个文件回调一次,
    ///   [total]=0 表示文件总数未知 (遍历未结束), [processed] 为已处理文件数;
    ///   UI 可据此显示"索引中 · 已发现 N 个文件", 随遍历逐渐增长
    /// - 引用解析阶段: (processed, 0, "resolve refs") 节流回调, 非完成信号
    /// - 完成信号:
    ///   - (total, total, "") 且 total>0 表示索引正常结束
    ///   - (0, 0, "") 表示无文件可索引 (同样视为结束)
    ///   (订阅方据此将状态置为 "完成" indexing=false)
    using IndexProgressCallback
        = std::function<void(int processed, int total, std::string_view current_file)>;
    void setProgressCallback(IndexProgressCallback callback);

private:

    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace agentxx_codegraph_plugin

#endif