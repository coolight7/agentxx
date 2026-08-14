#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#if AGENTXX_ENABLE_CODEGRAPH
#include "codegraph/core/json.hpp"
#include "codegraph/core/types.h"

namespace agentxx {
namespace expand {

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

class CodeGraphManager {
public:

    /// @param sqliteDir sqlite 数据目录; 为空使用默认 {dataDir}/sqlite/
    ///        (dataDir 为空时 ~/.agentxx/sqlite/, 取不到主目录时回退系统临时目录)
    explicit CodeGraphManager(std::string sqliteDir = "");
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

    using IndexProgressCallback
        = std::function<void(int processed, int total, std::string_view current_file)>;
    void setProgressCallback(IndexProgressCallback callback);

private:

    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace expand
} // namespace agentxx

#endif