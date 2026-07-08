#include "agentxx/expand/codegraph_manager.h"
#include "agentxx/agent/config_static.h"
#include "agentxx/util/log.h"
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <queue>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#if defined(AGENTXX_ENABLE_CODEGRAPH)
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

static constexpr std::string_view kCodeGraphDbPath = ".codegraph";

static bool should_skip(const std::string &file_path) {
  static const std::string kSkipAgentxx = fmt::format(
      "/{}/", agentxx::agent::AgentConfigStatic::agentxxDataDirPath);
  static const std::string kSkipCodeGraph =
      fmt::format("/{}/", kCodeGraphDbPath);

  fs::path p(file_path);
  std::string path_str = p.generic_string();
  return path_str.find("/.") != std::string::npos ||
         path_str.find("/node_modules/") != std::string::npos ||
         path_str.find("/build/") != std::string::npos ||
         path_str.find("/build-") != std::string::npos ||
         path_str.find("/__pycache__/") != std::string::npos ||
         path_str.find("/.git/") != std::string::npos ||
         path_str.find(kSkipAgentxx) != std::string::npos ||
         path_str.find(kSkipCodeGraph) != std::string::npos;
}

static std::vector<std::string>
collect_source_files(const std::string &root_path) {
  std::vector<std::string> files;
  try {
    for (const auto &entry : fs::recursive_directory_iterator(
             root_path, fs::directory_options::skip_permission_denied)) {
      if (!entry.is_regular_file())
        continue;
      std::string path = entry.path().generic_string();
      if (should_skip(path))
        continue;
      std::string lang = codegraph::detect_language(path);
      if (lang.empty())
        continue;
      files.push_back(path);
    }
  } catch (const std::exception &e) {
    XX_LOGE("CodeGraphManager: collect_source_files error: {}", e.what());
  }
  return files;
}

static bool is_changed(codegraph::Database &db,
                       const fs::directory_entry &entry,
                       const std::string &file_path) {
  auto existing = db.get_file(file_path);
  if (!existing.has_value())
    return true;
  try {
    auto ftime = fs::last_write_time(entry);
    auto mtime = std::chrono::duration_cast<std::chrono::seconds>(
                     ftime.time_since_epoch())
                     .count();
    return existing->mtime != mtime ||
           existing->size != static_cast<int64_t>(fs::file_size(entry));
  } catch (...) {
    return true;
  }
}

static int score_target(const codegraph::Node &source,
                        const codegraph::Node &candidate) {
  int score = 0;
  if (source.file_path == candidate.file_path) {
    score += 10;
  } else {
    auto src_dir = source.file_path.rfind('/');
    auto cand_dir = candidate.file_path.rfind('/');
    if (src_dir != std::string::npos && cand_dir != std::string::npos) {
      if (source.file_path.substr(0, src_dir) ==
          candidate.file_path.substr(0, cand_dir)) {
        score += 5;
      }
    }
  }
  if (!source.qualified_name.empty() && !candidate.qualified_name.empty()) {
    auto src_colon = source.qualified_name.rfind("::");
    auto cand_colon = candidate.qualified_name.rfind("::");
    if (src_colon != std::string::npos && cand_colon != std::string::npos) {
      std::string src_ns = source.qualified_name.substr(0, src_colon);
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
  Impl() : running_(false), needs_initialize_(true) {}
  ~Impl() { shutdown(); }

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

  bool initialize(const std::string &project_root) {
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
    fs::path cg_dir = fs::path(project_root) /
                      agentxx::agent::AgentConfigStatic::agentxxDataDirPath /
                      kCodeGraphDbPath;
    std::string index_path = (cg_dir / "index").string();

    try {
      if (!fs::exists(cg_dir)) {
        fs::create_directories(cg_dir);
      }
      db_ = std::make_unique<codegraph::Database>(index_path);
      db_->init_schema();
      traverser_ = std::make_unique<codegraph::GraphTraverser>(*db_);
      context_builder_ =
          std::make_unique<codegraph::ContextBuilder>(*db_, *traverser_);
      fts_search_ = std::make_unique<codegraph::FtsSearch>(*db_);
      needs_initialize_ = false;
      running_.store(true);
      return true;
    } catch (const std::exception &e) {
      XX_LOGE("CodeGraphManager: initialize failed: {}", e.what());
      return false;
    }
  }

  bool indexDirectory(const std::string &path, bool incremental) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_)
      return false;

    auto files = collect_source_files(path);
    if (files.empty()) {
      XX_LOGW("CodeGraphManager: no source files found in {}", path);
      return true;
    }

    XX_LOGI("CodeGraphManager: found {} source files to index", files.size());

    int processed = 0;
    int total = static_cast<int>(files.size());

    for (const auto &file_path : files) {
      if (!running_.load())
        break;

      XX_LOGI("CodeGraphManager: processing file [{}]: {}", processed,
              file_path);

      if (incremental) {
        try {
          fs::directory_entry entry(file_path);
          if (!is_changed(*db_, entry, file_path))
            continue;
        } catch (...) {
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

      auto extractor = codegraph::create_extractor(lang);
      if (!extractor) {
        XX_LOGW("CodeGraphManager: no extractor for lang={} file={}", lang,
                file_path);
        continue;
      }

      std::ifstream ifs(file_path);
      if (!ifs.is_open()) {
        XX_LOGW("CodeGraphManager: cannot open file {}", file_path);
        continue;
      }
      std::string source((std::istreambuf_iterator<char>(ifs)),
                         std::istreambuf_iterator<char>());

      auto result = extractor->extract(file_path, source);
      XX_LOGI(
          "CodeGraphManager: extracted {} nodes, {} unresolved refs from {}",
          result.nodes.size(), result.unresolved.size(), file_path);

      writeExtractionResult(file_path, lang, result);

      processed++;
    }

    resolveReferences();

    try {
      db_->rebuild_fts();
    } catch (const std::exception &e) {
      XX_LOGW("CodeGraphManager: FTS rebuild failed: {}", e.what());
    }

    return true;
  }

  bool updateIndex() {
    if (!db_)
      return false;
    return indexDirectory(project_root_, true);
  }

  bool resolveReferences() {
    if (!db_)
      return false;

    auto unresolved = db_->get_unresolved_refs();
    if (unresolved.empty())
      return true;

    db_->begin_transaction();
    try {
      for (const auto &ref : unresolved) {
        if (!running_.load())
          break;

        auto source_node = db_->get_node(ref.source_node_id);
        if (!source_node.has_value())
          continue;

        auto candidates = db_->find_nodes_by_name(ref.ref_name, 10);
        if (candidates.empty())
          continue;

        if (candidates.size() == 1) {
          codegraph::Edge edge;
          edge.source_id = source_node->id;
          edge.target_id = candidates[0].id;
          edge.kind = codegraph::EdgeKind::Calls;
          edge.line = ref.line;
          edge.col = ref.col;
          db_->insert_edge(edge);
        } else {
          codegraph::Node best = candidates[0];
          int best_score = -1;
          for (const auto &cand : candidates) {
            int s = score_target(source_node.value(), cand);
            if (s > best_score) {
              best_score = s;
              best = cand;
            }
          }
          codegraph::Edge edge;
          edge.source_id = source_node->id;
          edge.target_id = best.id;
          edge.kind = codegraph::EdgeKind::Calls;
          edge.line = ref.line;
          edge.col = ref.col;
          db_->insert_edge(edge);
        }

        db_->delete_unresolved_ref(ref.id);
      }
      db_->commit();
    } catch (const std::exception &e) {
      db_->rollback();
      XX_LOGE("CodeGraphManager: resolveReferences error: {}", e.what());
      return false;
    }

    return true;
  }

  CodeGraphSearchResult searchSymbols(const std::string &query, int limit) {
    CodeGraphSearchResult result;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_ || !fts_search_) {
      result.error = "CodeGraph not initialized";
      return result;
    }
    try {
      result.nodes = fts_search_->search(query, limit);
      result.success = true;
    } catch (const std::exception &e) {
      result.error = e.what();
    }
    return result;
  }

  CodeGraphContextResult getSymbolContext(const std::string &symbol, int limit,
                                          int max_depth) {
    CodeGraphContextResult result;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!context_builder_) {
      result.error = "CodeGraph not initialized";
      return result;
    }
    try {
      result.context =
          context_builder_->build_context(symbol, limit, max_depth);
      if (result.context.contains("error")) {
        result.error = result.context["error"].get<std::string>();
        result.success = false;
      } else {
        result.success = true;
      }
    } catch (const std::exception &e) {
      result.error = e.what();
    }
    return result;
  }

  CodeGraphImpactResult getCallers(const std::string &symbol, int max_depth) {
    CodeGraphImpactResult result;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!context_builder_) {
      result.error = "CodeGraph not initialized";
      return result;
    }
    try {
      result.impact = context_builder_->get_callers(symbol, max_depth);
      if (result.impact.contains("error")) {
        result.error = result.impact["error"].get<std::string>();
        result.success = false;
      } else {
        result.success = true;
      }
    } catch (const std::exception &e) {
      result.error = e.what();
    }
    return result;
  }

  CodeGraphImpactResult getCallees(const std::string &symbol, int max_depth) {
    CodeGraphImpactResult result;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!context_builder_) {
      result.error = "CodeGraph not initialized";
      return result;
    }
    try {
      result.impact = context_builder_->get_callees(symbol, max_depth);
      if (result.impact.contains("error")) {
        result.error = result.impact["error"].get<std::string>();
        result.success = false;
      } else {
        result.success = true;
      }
    } catch (const std::exception &e) {
      result.error = e.what();
    }
    return result;
  }

  CodeGraphImpactResult getImpact(const std::string &symbol, int max_depth) {
    CodeGraphImpactResult result;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!context_builder_) {
      result.error = "CodeGraph not initialized";
      return result;
    }
    try {
      result.impact = context_builder_->get_impact(symbol, max_depth);
      if (result.impact.contains("error")) {
        result.error = result.impact["error"].get<std::string>();
        result.success = false;
      } else {
        result.success = true;
      }
    } catch (const std::exception &e) {
      result.error = e.what();
    }
    return result;
  }

  CodeGraphPathResult findPath(const std::string &from, const std::string &to,
                               int max_depth) {
    CodeGraphPathResult result;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_ || !traverser_) {
      result.error = "CodeGraph not initialized";
      return result;
    }
    try {
      auto from_nodes = db_->find_nodes_by_name(from, 1);
      auto to_nodes = db_->find_nodes_by_name(to, 1);
      if (from_nodes.empty() || to_nodes.empty()) {
        result.error = "Symbol not found";
        return result;
      }
      auto path_ids =
          traverser_->find_path(from_nodes[0].id, to_nodes[0].id, max_depth);
      if (path_ids.empty()) {
        result.error = "No path found";
        return result;
      }
      auto nodes = db_->get_nodes_by_ids(path_ids);
      std::unordered_map<int64_t, codegraph::Node> node_map;
      for (auto &n : nodes) {
        node_map[n.id] = n;
      }
      for (auto id : path_ids) {
        auto it = node_map.find(id);
        if (it != node_map.end()) {
          result.path.push_back(it->second);
        }
      }
      result.success = true;
    } catch (const std::exception &e) {
      result.error = e.what();
    }
    return result;
  }

  CodeGraphStatusResult getStatus() {
    CodeGraphStatusResult result;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_ || !traverser_) {
      result.error = "CodeGraph not initialized";
      return result;
    }
    try {
      result.total_nodes = db_->count_nodes();
    } catch (const std::exception &e) {
      result.error = std::string("count_nodes: ") + e.what();
      return result;
    }
    try {
      result.total_edges = db_->count_edges();
    } catch (const std::exception &e) {
      result.error = std::string("count_edges: ") + e.what();
      return result;
    }
    try {
      result.total_files = static_cast<int64_t>(db_->get_all_files().size());
    } catch (const std::exception &e) {
      result.error = std::string("get_all_files: ") + e.what();
      return result;
    }
    try {
      auto cycles = traverser_->find_circular_dependencies();
      result.circular_deps = static_cast<int>(cycles.size());
    } catch (const std::exception &e) {
      result.error = std::string("find_circular_dependencies: ") + e.what();
      return result;
    }
    result.success = true;
    return result;
  }

  bool startFileWatcher(bool auto_reindex) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_ || file_watcher_running_)
      return false;

    try {
      file_watcher_ = codegraph::FileWatcher::create(project_root_, &running_);
      file_watcher_->add_watch_recursive(project_root_);

      file_watcher_->set_callback(
          [this, auto_reindex](const std::string &path, uint32_t mask) {
            if (auto_reindex && (mask & (codegraph::FILE_EVENT_MODIFIED |
                                         codegraph::FILE_EVENT_CREATED))) {
              std::string lang = codegraph::detect_language(path);
              if (!lang.empty() && !should_skip(path)) {
                this->indexFile(path, lang);
              }
            }
          });

      file_watcher_running_ = true;
      running_.store(true);
      worker_thread_ = std::thread([this]() {
        while (running_.load() && file_watcher_running_) {
          try {
            file_watcher_->poll(1000);
          } catch (const std::exception &e) {
            XX_LOGE("CodeGraphManager: file watcher poll error: {}", e.what());
          }
        }
        file_watcher_->stop();
      });
      return true;
    } catch (const std::exception &e) {
      XX_LOGE("CodeGraphManager: startFileWatcher error: {}", e.what());
      return false;
    }
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

  bool isRunning() const { return running_.load(); }

  void writeExtractionResult(const std::string &file_path,
                             const std::string &lang,
                             codegraph::ExtractionResult &result) {
    db_->begin_transaction();
    try {
      db_->delete_edges_for_file_nodes(file_path);
      db_->delete_unresolved_refs_by_file(file_path);
      db_->delete_nodes_by_file(file_path);

      std::vector<int64_t> id_map;
      id_map.reserve(result.nodes.size());
      for (auto &node : result.nodes) {
        if (node.kind == codegraph::NodeKind::File) {
          node.file_path = file_path;
        }
        int64_t id = db_->insert_node(node);
        id_map.push_back(id);
      }

      codegraph::FileRecord fr;
      fr.path = file_path;
      fr.language = lang;
      try {
        auto ftime = fs::last_write_time(fs::path(file_path));
        fr.mtime = std::chrono::duration_cast<std::chrono::seconds>(
                       ftime.time_since_epoch())
                       .count();
        fr.size = fs::file_size(fs::path(file_path));
      } catch (...) {
      }
      db_->insert_file(fr);

      for (auto ref : result.unresolved) {
        int original_index = static_cast<int>(-ref.source_node_id) - 1;
        if (original_index >= 0 &&
            original_index < static_cast<int>(id_map.size())) {
          ref.source_node_id = id_map[original_index];
        }
        db_->insert_unresolved_ref(ref);
      }

      db_->commit();
    } catch (const std::exception &e) {
      db_->rollback();
      XX_LOGE("CodeGraphManager: write error for {}: {}", file_path, e.what());
    }
  }

  void indexFile(const std::string &file_path, const std::string &lang) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_)
      return;

    auto extractor = codegraph::create_extractor(lang);
    if (!extractor)
      return;

    std::ifstream ifs(file_path);
    if (!ifs.is_open())
      return;
    std::string source((std::istreambuf_iterator<char>(ifs)),
                       std::istreambuf_iterator<char>());

    auto result = extractor->extract(file_path, source);
    writeExtractionResult(file_path, lang, result);
  }

  bool resolveReferencesLocked() {
    std::lock_guard<std::mutex> lock(mutex_);
    return resolveReferences();
  }

private:
  std::string project_root_;
  std::unique_ptr<codegraph::Database> db_;
  std::unique_ptr<codegraph::GraphTraverser> traverser_;
  std::unique_ptr<codegraph::ContextBuilder> context_builder_;
  std::unique_ptr<codegraph::FtsSearch> fts_search_;
  std::unique_ptr<codegraph::FileWatcher> file_watcher_;

  std::mutex mutex_;
  std::thread worker_thread_;
  std::condition_variable cv_;
  std::atomic<bool> running_;
  std::atomic<bool> file_watcher_running_{false};
  bool needs_initialize_;

  IndexProgressCallback progress_callback_;
};

CodeGraphManager::CodeGraphManager() : impl_(std::make_unique<Impl>()) {}
CodeGraphManager::~CodeGraphManager() { impl_->shutdown(); }

bool CodeGraphManager::initialize(const std::string &project_root) {
  return impl_->initialize(project_root);
}
void CodeGraphManager::shutdown() { impl_->shutdown(); }
bool CodeGraphManager::isRunning() const { return impl_->isRunning(); }
bool CodeGraphManager::indexDirectory(const std::string &path,
                                      bool incremental) {
  return impl_->indexDirectory(path, incremental);
}
bool CodeGraphManager::updateIndex() { return impl_->updateIndex(); }
bool CodeGraphManager::resolveReferences() {
  return impl_->resolveReferencesLocked();
}
CodeGraphSearchResult CodeGraphManager::searchSymbols(const std::string &query,
                                                      int limit) {
  return impl_->searchSymbols(query, limit);
}
CodeGraphContextResult
CodeGraphManager::getSymbolContext(const std::string &symbol, int limit,
                                   int max_depth) {
  return impl_->getSymbolContext(symbol, limit, max_depth);
}
CodeGraphImpactResult CodeGraphManager::getCallers(const std::string &symbol,
                                                   int max_depth) {
  return impl_->getCallers(symbol, max_depth);
}
CodeGraphImpactResult CodeGraphManager::getCallees(const std::string &symbol,
                                                   int max_depth) {
  return impl_->getCallees(symbol, max_depth);
}
CodeGraphImpactResult CodeGraphManager::getImpact(const std::string &symbol,
                                                  int max_depth) {
  return impl_->getImpact(symbol, max_depth);
}
CodeGraphPathResult CodeGraphManager::findPath(const std::string &from,
                                               const std::string &to,
                                               int max_depth) {
  return impl_->findPath(from, to, max_depth);
}
CodeGraphStatusResult CodeGraphManager::getStatus() {
  return impl_->getStatus();
}
bool CodeGraphManager::startFileWatcher(bool auto_reindex) {
  return impl_->startFileWatcher(auto_reindex);
}
void CodeGraphManager::stopFileWatcher() { impl_->stopFileWatcher(); }
void CodeGraphManager::setProgressCallback(IndexProgressCallback callback) {
  impl_->setProgressCallback(std::move(callback));
}

} // namespace expand
} // namespace agentxx

#endif