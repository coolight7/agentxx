#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if AGENTXX_ENABLE_CODEGRAPH
#include "codegraph/core/json.hpp"
#include "codegraph/core/types.h"

namespace agentxx {
namespace expand {

namespace fs = std::filesystem;

struct CodeGraphSearchResult {
  std::vector<codegraph::Node> nodes;
  std::string error;
  bool success = false;
};

struct CodeGraphContextResult {
  codegraph::Json context;
  std::string error;
  bool success = false;
};

struct CodeGraphStatusResult {
  int64_t total_nodes = 0;
  int64_t total_edges = 0;
  int64_t total_files = 0;
  int circular_deps = 0;
  std::string error;
  bool success = false;
};

struct CodeGraphImpactResult {
  codegraph::Json impact;
  std::string error;
  bool success = false;
};

struct CodeGraphPathResult {
  std::vector<codegraph::Node> path;
  std::string error;
  bool success = false;
};

class CodeGraphManager {
public:
  CodeGraphManager();
  ~CodeGraphManager();

  CodeGraphManager(const CodeGraphManager &) = delete;
  CodeGraphManager &operator=(const CodeGraphManager &) = delete;

  bool initialize(const std::string &project_root);
  void shutdown();

  bool isRunning() const;

  bool indexDirectory(const std::string &path, bool incremental = true);
  bool updateIndex();
  bool resolveReferences();

  CodeGraphSearchResult searchSymbols(const std::string &query, int limit = 20);
  CodeGraphContextResult getSymbolContext(const std::string &symbol,
                                          int limit = 10, int max_depth = 3);
  CodeGraphImpactResult getCallers(const std::string &symbol,
                                   int max_depth = 3);
  CodeGraphImpactResult getCallees(const std::string &symbol,
                                   int max_depth = 3);
  CodeGraphImpactResult getImpact(const std::string &symbol, int max_depth = 5);
  CodeGraphPathResult findPath(const std::string &from, const std::string &to,
                               int max_depth = 10);
  CodeGraphStatusResult getStatus();

  bool startFileWatcher(bool auto_reindex = true);
  void stopFileWatcher();

  using IndexProgressCallback = std::function<void(
      int processed, int total, const std::string &current_file)>;
  void setProgressCallback(IndexProgressCallback callback);

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace expand
} // namespace agentxx

#endif