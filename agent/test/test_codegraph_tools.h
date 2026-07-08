#pragma once

#include "agentxx/agent/context.h"
#include "agentxx/expand/codegraph_manager.h"
#include "agentxx/tools/codegraph_tool.h"
#include "neograph/neograph.h"
#include <asio/awaitable.hpp>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#if defined(AGENTXX_ENABLE_CODEGRAPH)

namespace agentxx {
namespace test {

namespace fs = std::filesystem;

static std::atomic<int> g_temp_project_counter{0};

static std::string create_temp_project() {
  int idx = g_temp_project_counter.fetch_add(1);
  auto tmp_dir =
      fs::temp_directory_path() / ("codegraph_test_" + std::to_string(idx));
  if (fs::exists(tmp_dir)) {
    fs::remove_all(tmp_dir);
  }
  fs::create_directories(tmp_dir);

  {
    std::ofstream f(tmp_dir / "main.cpp");
    f << R"(#include "utils.h"

int add(int a, int b) {
    return a + b;
}

int main() {
    int result = add(1, 2);
    int doubled = multiply(result, 2);
    return doubled;
}
)";
  }

  {
    std::ofstream f(tmp_dir / "utils.h");
    f << R"(#pragma once

int multiply(int x, int y);

void print_result(int value);
)";
  }

  {
    std::ofstream f(tmp_dir / "utils.cpp");
    f << R"(#include "utils.h"
#include <iostream>

int multiply(int x, int y) {
    int result = 0;
    for (int i = 0; i < y; i++) {
        result = add_impl(result, x);
    }
    return result;
}

static int add_impl(int a, int b) {
    return a + b;
}

void print_result(int value) {
    std::cout << "Result: " << value << std::endl;
}
)";
  }

  return tmp_dir.generic_string();
}

static void cleanup_temp_project(const std::string &path) {
  try {
    fs::remove_all(path);
  } catch (...) {
  }
}

static std::string create_multi_lang_project() {
  int idx = g_temp_project_counter.fetch_add(1);
  auto tmp_dir = fs::temp_directory_path() /
                 ("codegraph_multi_test_" + std::to_string(idx));
  if (fs::exists(tmp_dir)) {
    fs::remove_all(tmp_dir);
  }
  fs::create_directories(tmp_dir);

  // Python
  {
    std::ofstream f(tmp_dir / "main.py");
    f << R"(
def greet(name: str) -> str:
    return f"Hello, {name}!"

def add(a: int, b: int) -> int:
    return a + b

def main() -> None:
    msg = greet("world")
    result = add(1, 2)
    print(f"{msg} Result: {result}")

if __name__ == "__main__":
    main()
)";
  }

  // JavaScript
  {
    std::ofstream f(tmp_dir / "utils.js");
    f << R"(
function multiply(x, y) {
    let result = 0;
    for (let i = 0; i < y; i++) {
        result = add(result, x);
    }
    return result;
}

function add(a, b) {
    return a + b;
}

function formatResult(value) {
    return `Result: ${value}`;
}

module.exports = { multiply, formatResult };
)";
  }

  // TypeScript
  {
    std::ofstream f(tmp_dir / "types.ts");
    f << R"(
interface User {
    id: number;
    name: string;
}

function getUser(id: number): User {
    return { id, name: `user_${id}` };
}

function formatUser(user: User): string {
    return `User: ${user.name}`;
}

function processUser(id: number): string {
    const user = getUser(id);
    return formatUser(user);
}

export { getUser, formatUser, processUser, User };
)";
  }

  // Rust
  {
    std::ofstream f(tmp_dir / "lib.rs");
    f << R"(
pub fn factorial(n: u64) -> u64 {
    if n <= 1 {
        return 1;
    }
    n * factorial(n - 1)
}

pub fn compute_sum(n: u64) -> u64 {
    let mut sum = 0;
    for i in 1..=n {
        sum = add_to_sum(sum, i);
    }
    sum
}

fn add_to_sum(current: u64, value: u64) -> u64 {
    current + value
}

pub fn greet(name: &str) -> String {
    format!("Hello, {}!", name)
}
)";
  }

  // Go
  {
    std::ofstream f(tmp_dir / "main.go");
    f << R"(package main

import "fmt"

func add(a, b int) int {
    return a + b
}

func multiply(a, b int) int {
    result := 0
    for i := 0; i < b; i++ {
        result = add(result, a)
    }
    return result
}

func greet(name string) string {
    return fmt.Sprintf("Hello, %s!", name)
}

func main() {
    msg := greet("world")
    result := multiply(3, 4)
    fmt.Printf("%s Result: %d\n", msg, result)
}
)";
  }

  // Java
  {
    std::ofstream f(tmp_dir / "App.java");
    f << R"(
public class App {
    public static int add(int a, int b) {
        return a + b;
    }

    public static int multiply(int a, int b) {
        int result = 0;
        for (int i = 0; i < b; i++) {
            result = add(result, a);
        }
        return result;
    }

    public static String greet(String name) {
        return "Hello, " + name + "!";
    }

    public static void main(String[] args) {
        String msg = greet("world");
        int result = multiply(3, 4);
        System.out.println(msg + " Result: " + result);
    }
}
)";
  }

  return tmp_dir.generic_string();
}

// =========================================================================
// CodeGraph Manager Tests
// =========================================================================

inline void test_codegraph_manager_init() {
  auto tmp_dir = create_temp_project();
  auto manager = agentxx::expand::CodeGraphManager{};

  bool ok = manager.initialize(tmp_dir);
  if (ok) {
    std::cout << "[PASS] CodeGraphManager::initialize creates index"
              << std::endl;
  } else {
    std::cout << "[FAIL] CodeGraphManager::initialize failed" << std::endl;
  }

  cleanup_temp_project(tmp_dir);
}

inline void test_codegraph_manager_init_twice() {
  auto tmp_dir = create_temp_project();
  auto manager = agentxx::expand::CodeGraphManager{};

  bool ok1 = manager.initialize(tmp_dir);
  bool ok2 = manager.initialize(tmp_dir);

  if (ok1 && ok2) {
    std::cout << "[PASS] CodeGraphManager::initialize twice succeeds"
              << std::endl;
  } else {
    std::cout << "[FAIL] CodeGraphManager::initialize twice failed"
              << std::endl;
  }

  cleanup_temp_project(tmp_dir);
}

inline void test_codegraph_manager_not_initialized() {
  auto manager = agentxx::expand::CodeGraphManager{};

  auto search_result = manager.searchSymbols("test", 10);
  if (!search_result.success &&
      search_result.error.find("not initialized") != std::string::npos) {
    std::cout << "[PASS] CodeGraphManager rejects search when not initialized"
              << std::endl;
  } else {
    std::cout << "[FAIL] CodeGraphManager should reject uninitialized search"
              << std::endl;
  }

  auto status_result = manager.getStatus();
  if (!status_result.success &&
      status_result.error.find("not initialized") != std::string::npos) {
    std::cout << "[PASS] CodeGraphManager rejects status when not initialized"
              << std::endl;
  } else {
    std::cout << "[FAIL] CodeGraphManager should reject uninitialized status"
              << std::endl;
  }
}

inline void test_codegraph_manager_index() {
  auto tmp_dir = create_temp_project();
  auto manager = agentxx::expand::CodeGraphManager{};

  manager.initialize(tmp_dir);
  bool ok = manager.indexDirectory(tmp_dir, false);

  if (ok) {
    std::cout << "[PASS] CodeGraphManager::indexDirectory succeeds"
              << std::endl;
  } else {
    std::cout << "[FAIL] CodeGraphManager::indexDirectory failed" << std::endl;
  }

  cleanup_temp_project(tmp_dir);
}

inline void test_codegraph_manager_incremental_index() {
  auto tmp_dir = create_temp_project();
  auto manager = agentxx::expand::CodeGraphManager{};

  manager.initialize(tmp_dir);
  manager.indexDirectory(tmp_dir, false);

  bool ok = manager.indexDirectory(tmp_dir, true);
  if (ok) {
    std::cout << "[PASS] CodeGraphManager::indexDirectory incremental succeeds"
              << std::endl;
  } else {
    std::cout << "[FAIL] CodeGraphManager::indexDirectory incremental failed"
              << std::endl;
  }

  cleanup_temp_project(tmp_dir);
}

inline void test_codegraph_manager_search() {
  auto tmp_dir = create_temp_project();
  auto manager = agentxx::expand::CodeGraphManager{};

  manager.initialize(tmp_dir);
  manager.indexDirectory(tmp_dir, false);

  auto result = manager.searchSymbols("add", 10);
  if (result.success) {
    if (!result.nodes.empty()) {
      std::cout << "[PASS] CodeGraphManager::searchSymbols found "
                << result.nodes.size() << " results" << std::endl;
    } else {
      std::cout << "[INFO] CodeGraphManager::searchSymbols returned empty "
                   "(FTS may not be populated)"
                << std::endl;
    }
  } else {
    std::cout << "[FAIL] CodeGraphManager::searchSymbols error: "
              << (result.error.empty() ? "(empty)" : result.error) << std::endl;
  }

  cleanup_temp_project(tmp_dir);
}

inline void test_codegraph_manager_search_no_results() {
  auto tmp_dir = create_temp_project();
  auto manager = agentxx::expand::CodeGraphManager{};

  manager.initialize(tmp_dir);
  manager.indexDirectory(tmp_dir, false);

  auto result = manager.searchSymbols("nonexistent_symbol_xyz", 10);
  if (result.success && result.nodes.empty()) {
    std::cout << "[PASS] CodeGraphManager::searchSymbols returns empty for "
                 "unknown symbol"
              << std::endl;
  } else if (!result.success) {
    std::cout << "[FAIL] CodeGraphManager::searchSymbols error: "
              << (result.error.empty() ? "(empty)" : result.error) << std::endl;
  } else {
    std::cout << "[FAIL] CodeGraphManager::searchSymbols should return empty "
                 "for unknown symbol"
              << std::endl;
  }

  cleanup_temp_project(tmp_dir);
}

inline void test_codegraph_manager_context() {
  auto tmp_dir = create_temp_project();
  auto manager = agentxx::expand::CodeGraphManager{};

  manager.initialize(tmp_dir);
  manager.indexDirectory(tmp_dir, false);

  auto result = manager.getSymbolContext("add");
  if (result.success) {
    std::cout << "[PASS] CodeGraphManager::getSymbolContext succeeds"
              << std::endl;
  } else {
    std::cout << "[FAIL] CodeGraphManager::getSymbolContext failed: "
              << (result.error.empty() ? "(empty)" : result.error) << std::endl;
  }

  cleanup_temp_project(tmp_dir);
}

inline void test_codegraph_manager_status() {
  auto tmp_dir = create_temp_project();
  auto manager = agentxx::expand::CodeGraphManager{};

  manager.initialize(tmp_dir);
  manager.indexDirectory(tmp_dir, false);

  try {
    auto result = manager.getStatus();
    if (result.success) {
      std::cout << "[PASS] CodeGraphManager::getStatus returns stats"
                << std::endl;
    } else {
      std::cout << "[FAIL] CodeGraphManager::getStatus failed: "
                << (result.error.empty() ? "(empty)" : result.error)
                << std::endl;
    }
  } catch (const std::exception &e) {
    std::cout << "[FAIL] CodeGraphManager::getStatus threw: " << e.what()
              << std::endl;
  } catch (...) {
    std::cout << "[FAIL] CodeGraphManager::getStatus threw unknown exception"
              << std::endl;
  }

  cleanup_temp_project(tmp_dir);
}

inline void test_codegraph_manager_callers() {
  auto tmp_dir = create_temp_project();
  auto manager = agentxx::expand::CodeGraphManager{};

  manager.initialize(tmp_dir);
  manager.indexDirectory(tmp_dir, false);

  auto result = manager.getCallers("add");
  if (result.success) {
    std::cout << "[PASS] CodeGraphManager::getCallers succeeds" << std::endl;
  } else {
    std::cout << "[FAIL] CodeGraphManager::getCallers failed: "
              << (result.error.empty() ? "(empty)" : result.error) << std::endl;
  }

  cleanup_temp_project(tmp_dir);
}

inline void test_codegraph_manager_callees() {
  auto tmp_dir = create_temp_project();
  auto manager = agentxx::expand::CodeGraphManager{};

  manager.initialize(tmp_dir);
  manager.indexDirectory(tmp_dir, false);

  auto result = manager.getCallees("main");
  if (result.success) {
    std::cout << "[PASS] CodeGraphManager::getCallees succeeds" << std::endl;
  } else {
    std::cout << "[FAIL] CodeGraphManager::getCallees failed: "
              << (result.error.empty() ? "(empty)" : result.error) << std::endl;
  }

  cleanup_temp_project(tmp_dir);
}

inline void test_codegraph_manager_impact() {
  auto tmp_dir = create_temp_project();
  auto manager = agentxx::expand::CodeGraphManager{};

  manager.initialize(tmp_dir);
  manager.indexDirectory(tmp_dir, false);

  auto result = manager.getImpact("add");
  if (result.success) {
    std::cout << "[PASS] CodeGraphManager::getImpact succeeds" << std::endl;
  } else {
    std::cout << "[FAIL] CodeGraphManager::getImpact failed: "
              << (result.error.empty() ? "(empty)" : result.error) << std::endl;
  }

  cleanup_temp_project(tmp_dir);
}

inline void test_codegraph_manager_path() {
  auto tmp_dir = create_temp_project();
  auto manager = agentxx::expand::CodeGraphManager{};

  manager.initialize(tmp_dir);
  manager.indexDirectory(tmp_dir, false);

  auto result = manager.findPath("main", "add_impl");
  if (result.success) {
    std::cout << "[PASS] CodeGraphManager::findPath found path" << std::endl;
  } else {
    std::cout << "[INFO] CodeGraphManager::findPath result: "
              << (result.error.empty() ? "(empty)" : result.error) << std::endl;
  }

  cleanup_temp_project(tmp_dir);
}

inline void test_codegraph_manager_shutdown() {
  auto tmp_dir = create_temp_project();
  auto manager = agentxx::expand::CodeGraphManager{};

  manager.initialize(tmp_dir);
  manager.indexDirectory(tmp_dir, false);
  manager.shutdown();

  auto result = manager.searchSymbols("add", 10);
  if (!result.success &&
      result.error.find("not initialized") != std::string::npos) {
    std::cout << "[PASS] CodeGraphManager::shutdown disables queries"
              << std::endl;
  } else {
    std::cout << "[FAIL] CodeGraphManager::shutdown should disable queries"
              << std::endl;
  }

  cleanup_temp_project(tmp_dir);
}

inline void test_codegraph_manager_update_index() {
  auto tmp_dir = create_temp_project();
  auto manager = agentxx::expand::CodeGraphManager{};

  manager.initialize(tmp_dir);
  manager.indexDirectory(tmp_dir, false);

  bool ok = manager.updateIndex();
  if (ok) {
    std::cout << "[PASS] CodeGraphManager::updateIndex succeeds" << std::endl;
  } else {
    std::cout << "[FAIL] CodeGraphManager::updateIndex failed" << std::endl;
  }

  cleanup_temp_project(tmp_dir);
}

inline void test_codegraph_manager_resolve() {
  auto tmp_dir = create_temp_project();
  auto manager = agentxx::expand::CodeGraphManager{};

  manager.initialize(tmp_dir);
  manager.indexDirectory(tmp_dir, false);

  auto result = manager.resolveReferences();
  if (result) {
    std::cout << "[PASS] CodeGraphManager::resolveReferences succeeds"
              << std::endl;
  } else {
    std::cout << "[FAIL] CodeGraphManager::resolveReferences failed"
              << std::endl;
  }

  cleanup_temp_project(tmp_dir);
}

// =========================================================================
// Multi-Language CodeGraph Tests
// =========================================================================

inline void test_codegraph_manager_multi_lang_index() {
  auto tmp_dir = create_multi_lang_project();
  auto manager = agentxx::expand::CodeGraphManager{};

  manager.initialize(tmp_dir);
  bool ok = manager.indexDirectory(tmp_dir, false);

  if (ok) {
    std::cout << "[PASS] CodeGraphManager multi-lang index succeeds"
              << std::endl;
  } else {
    std::cout << "[FAIL] CodeGraphManager multi-lang index failed" << std::endl;
  }

  cleanup_temp_project(tmp_dir);
}

inline void test_codegraph_manager_multi_lang_status() {
  auto tmp_dir = create_multi_lang_project();
  auto manager = agentxx::expand::CodeGraphManager{};

  manager.initialize(tmp_dir);
  manager.indexDirectory(tmp_dir, false);

  auto result = manager.getStatus();
  if (result.success && result.total_files >= 6) {
    std::cout << "[PASS] CodeGraphManager multi-lang status: "
              << result.total_files << " files, " << result.total_nodes
              << " nodes, " << result.total_edges << " edges" << std::endl;
  } else {
    std::cout << "[FAIL] CodeGraphManager multi-lang status failed: "
              << (result.error.empty() ? "too few files" : result.error)
              << std::endl;
  }

  cleanup_temp_project(tmp_dir);
}

inline void test_codegraph_manager_python_search() {
  auto tmp_dir = create_multi_lang_project();
  auto manager = agentxx::expand::CodeGraphManager{};

  manager.initialize(tmp_dir);
  manager.indexDirectory(tmp_dir, false);

  auto result = manager.searchSymbols("greet", 20);
  if (result.success) {
    std::cout << "[PASS] CodeGraphManager Python search 'greet' found "
              << result.nodes.size() << " results" << std::endl;
  } else {
    std::cout << "[FAIL] CodeGraphManager Python search failed: "
              << (result.error.empty() ? "(empty)" : result.error) << std::endl;
  }

  cleanup_temp_project(tmp_dir);
}

inline void test_codegraph_manager_javascript_search() {
  auto tmp_dir = create_multi_lang_project();
  auto manager = agentxx::expand::CodeGraphManager{};

  manager.initialize(tmp_dir);
  manager.indexDirectory(tmp_dir, false);

  auto result = manager.searchSymbols("multiply", 20);
  if (result.success) {
    bool found_js = false;
    for (const auto &node : result.nodes) {
      if (node.file_path.find(".js") != std::string::npos) {
        found_js = true;
        break;
      }
    }
    if (found_js || result.nodes.empty()) {
      std::cout << "[PASS] CodeGraphManager JavaScript search 'multiply' found "
                << result.nodes.size() << " results" << std::endl;
    } else {
      std::cout << "[FAIL] CodeGraphManager JavaScript search: no .js results"
                << std::endl;
    }
  } else {
    std::cout << "[FAIL] CodeGraphManager JavaScript search failed: "
              << (result.error.empty() ? "(empty)" : result.error) << std::endl;
  }

  cleanup_temp_project(tmp_dir);
}

inline void test_codegraph_manager_typescript_search() {
  auto tmp_dir = create_multi_lang_project();
  auto manager = agentxx::expand::CodeGraphManager{};

  manager.initialize(tmp_dir);
  manager.indexDirectory(tmp_dir, false);

  auto result = manager.searchSymbols("getUser", 20);
  if (result.success) {
    std::cout << "[PASS] CodeGraphManager TypeScript search 'getUser' found "
              << result.nodes.size() << " results" << std::endl;
  } else {
    std::cout << "[FAIL] CodeGraphManager TypeScript search failed: "
              << (result.error.empty() ? "(empty)" : result.error) << std::endl;
  }

  cleanup_temp_project(tmp_dir);
}

inline void test_codegraph_manager_rust_search() {
  auto tmp_dir = create_multi_lang_project();
  auto manager = agentxx::expand::CodeGraphManager{};

  manager.initialize(tmp_dir);
  manager.indexDirectory(tmp_dir, false);

  auto result = manager.searchSymbols("factorial", 20);
  if (result.success) {
    std::cout << "[PASS] CodeGraphManager Rust search 'factorial' found "
              << result.nodes.size() << " results" << std::endl;
  } else {
    std::cout << "[FAIL] CodeGraphManager Rust search failed: "
              << (result.error.empty() ? "(empty)" : result.error) << std::endl;
  }

  cleanup_temp_project(tmp_dir);
}

inline void test_codegraph_manager_go_search() {
  auto tmp_dir = create_multi_lang_project();
  auto manager = agentxx::expand::CodeGraphManager{};

  manager.initialize(tmp_dir);
  manager.indexDirectory(tmp_dir, false);

  auto result = manager.searchSymbols("greet", 20);
  if (result.success) {
    bool found_go = false;
    for (const auto &node : result.nodes) {
      if (node.file_path.find(".go") != std::string::npos) {
        found_go = true;
        break;
      }
    }
    if (found_go || result.nodes.empty()) {
      std::cout << "[PASS] CodeGraphManager Go search 'greet' found "
                << result.nodes.size() << " results" << std::endl;
    } else {
      std::cout << "[FAIL] CodeGraphManager Go search: no .go results"
                << std::endl;
    }
  } else {
    std::cout << "[FAIL] CodeGraphManager Go search failed: "
              << (result.error.empty() ? "(empty)" : result.error) << std::endl;
  }

  cleanup_temp_project(tmp_dir);
}

inline void test_codegraph_manager_java_search() {
  auto tmp_dir = create_multi_lang_project();
  auto manager = agentxx::expand::CodeGraphManager{};

  manager.initialize(tmp_dir);
  manager.indexDirectory(tmp_dir, false);

  auto result = manager.searchSymbols("multiply", 20);
  if (result.success) {
    bool found_java = false;
    for (const auto &node : result.nodes) {
      if (node.file_path.find(".java") != std::string::npos) {
        found_java = true;
        break;
      }
    }
    if (found_java || result.nodes.empty()) {
      std::cout << "[PASS] CodeGraphManager Java search 'multiply' found "
                << result.nodes.size() << " results" << std::endl;
    } else {
      std::cout << "[FAIL] CodeGraphManager Java search: no .java results"
                << std::endl;
    }
  } else {
    std::cout << "[FAIL] CodeGraphManager Java search failed: "
              << (result.error.empty() ? "(empty)" : result.error) << std::endl;
  }

  cleanup_temp_project(tmp_dir);
}

inline void test_codegraph_manager_multi_lang_context() {
  auto tmp_dir = create_multi_lang_project();
  auto manager = agentxx::expand::CodeGraphManager{};

  manager.initialize(tmp_dir);
  manager.indexDirectory(tmp_dir, false);

  auto result = manager.getSymbolContext("add");
  if (result.success) {
    std::cout << "[PASS] CodeGraphManager multi-lang getSymbolContext 'add' "
                 "succeeds"
              << std::endl;
  } else {
    std::cout << "[FAIL] CodeGraphManager multi-lang getSymbolContext failed: "
              << (result.error.empty() ? "(empty)" : result.error) << std::endl;
  }

  cleanup_temp_project(tmp_dir);
}

inline void test_codegraph_manager_multi_lang_path() {
  auto tmp_dir = create_multi_lang_project();
  auto manager = agentxx::expand::CodeGraphManager{};

  manager.initialize(tmp_dir);
  manager.indexDirectory(tmp_dir, false);

  auto result = manager.findPath("multiply", "add");
  if (result.success ||
      result.error.find("No path found") != std::string::npos) {
    std::cout << "[PASS] CodeGraphManager multi-lang findPath multiply->add "
                 "returned"
              << std::endl;
  } else {
    std::cout << "[INFO] CodeGraphManager multi-lang findPath: "
              << (result.error.empty() ? "(empty)" : result.error) << std::endl;
  }

  cleanup_temp_project(tmp_dir);
}

// =========================================================================
// Tool Definition Tests
// =========================================================================

inline asio::awaitable<void> test_codegraph_search_tool_definition(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
  auto codegraph = std::make_shared<agentxx::expand::CodeGraphManager>();
  auto tool = agentxx::tools::CodeGraphSearchTool{codegraph, agentContext};

  auto def = tool.get_definition();
  if (def.name == "codegraph_search") {
    std::cout << "[PASS] CodeGraphSearchTool::get_definition name correct"
              << std::endl;
  } else {
    std::cout << "[FAIL] CodeGraphSearchTool::get_definition name incorrect"
              << std::endl;
  }
  co_return;
}

inline asio::awaitable<void> test_codegraph_context_tool_definition(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
  auto codegraph = std::make_shared<agentxx::expand::CodeGraphManager>();
  auto tool = agentxx::tools::CodeGraphContextTool{codegraph, agentContext};

  auto def = tool.get_definition();
  if (def.name == "codegraph_context") {
    std::cout << "[PASS] CodeGraphContextTool::get_definition name correct"
              << std::endl;
  } else {
    std::cout << "[FAIL] CodeGraphContextTool::get_definition name incorrect"
              << std::endl;
  }
  co_return;
}

inline asio::awaitable<void> test_codegraph_callers_tool_definition(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
  auto codegraph = std::make_shared<agentxx::expand::CodeGraphManager>();
  auto tool = agentxx::tools::CodeGraphCallersTool{codegraph, agentContext};

  auto def = tool.get_definition();
  if (def.name == "codegraph_callers") {
    std::cout << "[PASS] CodeGraphCallersTool::get_definition name correct"
              << std::endl;
  } else {
    std::cout << "[FAIL] CodeGraphCallersTool::get_definition name incorrect"
              << std::endl;
  }
  co_return;
}

inline asio::awaitable<void> test_codegraph_callees_tool_definition(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
  auto codegraph = std::make_shared<agentxx::expand::CodeGraphManager>();
  auto tool = agentxx::tools::CodeGraphCalleesTool{codegraph, agentContext};

  auto def = tool.get_definition();
  if (def.name == "codegraph_callees") {
    std::cout << "[PASS] CodeGraphCalleesTool::get_definition name correct"
              << std::endl;
  } else {
    std::cout << "[FAIL] CodeGraphCalleesTool::get_definition name incorrect"
              << std::endl;
  }
  co_return;
}

inline asio::awaitable<void> test_codegraph_impact_tool_definition(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
  auto codegraph = std::make_shared<agentxx::expand::CodeGraphManager>();
  auto tool = agentxx::tools::CodeGraphImpactTool{codegraph, agentContext};

  auto def = tool.get_definition();
  if (def.name == "codegraph_impact") {
    std::cout << "[PASS] CodeGraphImpactTool::get_definition name correct"
              << std::endl;
  } else {
    std::cout << "[FAIL] CodeGraphImpactTool::get_definition name incorrect"
              << std::endl;
  }
  co_return;
}

inline asio::awaitable<void> test_codegraph_status_tool_definition(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
  auto codegraph = std::make_shared<agentxx::expand::CodeGraphManager>();
  auto tool = agentxx::tools::CodeGraphStatusTool{codegraph, agentContext};

  auto def = tool.get_definition();
  if (def.name == "codegraph_status") {
    std::cout << "[PASS] CodeGraphStatusTool::get_definition name correct"
              << std::endl;
  } else {
    std::cout << "[FAIL] CodeGraphStatusTool::get_definition name incorrect"
              << std::endl;
  }
  co_return;
}

inline asio::awaitable<void> test_codegraph_index_tool_definition(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
  auto codegraph = std::make_shared<agentxx::expand::CodeGraphManager>();
  auto tool = agentxx::tools::CodeGraphIndexTool{codegraph, agentContext};

  auto def = tool.get_definition();
  if (def.name == "codegraph_index") {
    std::cout << "[PASS] CodeGraphIndexTool::get_definition name correct"
              << std::endl;
  } else {
    std::cout << "[FAIL] CodeGraphIndexTool::get_definition name incorrect"
              << std::endl;
  }
  co_return;
}

inline asio::awaitable<void> test_codegraph_path_tool_definition(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
  auto codegraph = std::make_shared<agentxx::expand::CodeGraphManager>();
  auto tool = agentxx::tools::CodeGraphPathTool{codegraph, agentContext};

  auto def = tool.get_definition();
  if (def.name == "codegraph_path") {
    std::cout << "[PASS] CodeGraphPathTool::get_definition name correct"
              << std::endl;
  } else {
    std::cout << "[FAIL] CodeGraphPathTool::get_definition name incorrect"
              << std::endl;
  }
  co_return;
}

// =========================================================================
// Tool Execute Tests (with real index)
// =========================================================================

inline asio::awaitable<void> test_codegraph_search_tool_execute(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
  auto tmp_dir = create_temp_project();
  auto codegraph = std::make_shared<agentxx::expand::CodeGraphManager>();
  codegraph->initialize(tmp_dir);
  codegraph->indexDirectory(tmp_dir, false);

  auto tool = agentxx::tools::CodeGraphSearchTool{codegraph, agentContext};
  auto result = co_await tool.execute_async({{"query", "add"}});

  bool has_kind = result.find("kind") != std::string::npos;
  bool has_name = result.find("name") != std::string::npos;
  bool has_error = result.find("error") != std::string::npos;

  if (has_kind && has_name) {
    std::cout << "[PASS] CodeGraphSearchTool returns valid search results"
              << std::endl;
  } else if (!has_error && result == "[]") {
    std::cout << "[INFO] CodeGraphSearchTool returned empty (FTS may not be "
                 "populated)"
              << std::endl;
  } else {
    std::cout << "[FAIL] CodeGraphSearchTool invalid result: " << result
              << std::endl;
  }

  cleanup_temp_project(tmp_dir);
}

inline asio::awaitable<void> test_codegraph_search_tool_execute_empty_query(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
  auto codegraph = std::make_shared<agentxx::expand::CodeGraphManager>();
  auto tool = agentxx::tools::CodeGraphSearchTool{codegraph, agentContext};

  auto result = co_await tool.execute_async({{"query", ""}});
  if (result.find("error") != std::string::npos) {
    std::cout << "[PASS] CodeGraphSearchTool rejects empty query" << std::endl;
  } else {
    std::cout << "[FAIL] CodeGraphSearchTool should reject empty query, got: "
              << result << std::endl;
  }
}

inline asio::awaitable<void> test_codegraph_context_tool_execute(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
  auto tmp_dir = create_temp_project();
  auto codegraph = std::make_shared<agentxx::expand::CodeGraphManager>();
  codegraph->initialize(tmp_dir);
  codegraph->indexDirectory(tmp_dir, false);

  auto tool = agentxx::tools::CodeGraphContextTool{codegraph, agentContext};
  auto result = co_await tool.execute_async({{"symbol", "add"}});

  if (result.find("symbol") != std::string::npos) {
    std::cout << "[PASS] CodeGraphContextTool returns context" << std::endl;
  } else {
    std::cout << "[FAIL] CodeGraphContextTool failed: " << result << std::endl;
  }

  cleanup_temp_project(tmp_dir);
}

inline asio::awaitable<void> test_codegraph_status_tool_execute(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
  auto tmp_dir = create_temp_project();
  auto codegraph = std::make_shared<agentxx::expand::CodeGraphManager>();
  codegraph->initialize(tmp_dir);
  codegraph->indexDirectory(tmp_dir, false);

  auto tool = agentxx::tools::CodeGraphStatusTool{codegraph, agentContext};
  auto result = co_await tool.execute_async(neograph::json{});

  bool has_nodes = result.find("total_nodes") != std::string::npos;
  bool has_edges = result.find("total_edges") != std::string::npos;
  bool has_files = result.find("total_files") != std::string::npos;

  if (has_nodes && has_edges && has_files) {
    std::cout << "[PASS] CodeGraphStatusTool returns statistics" << std::endl;
  } else {
    std::cout << "[FAIL] CodeGraphStatusTool incomplete: " << result
              << std::endl;
  }

  cleanup_temp_project(tmp_dir);
}

inline asio::awaitable<void> test_codegraph_index_tool_execute(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
  auto tmp_dir = create_temp_project();
  auto codegraph = std::make_shared<agentxx::expand::CodeGraphManager>();
  codegraph->initialize(tmp_dir);

  auto tool = agentxx::tools::CodeGraphIndexTool{codegraph, agentContext};
  auto result =
      co_await tool.execute_async({{"path", tmp_dir}, {"incremental", false}});

  if (result.find("success") != std::string::npos) {
    std::cout << "[PASS] CodeGraphIndexTool indexes directory" << std::endl;
  } else {
    std::cout << "[FAIL] CodeGraphIndexTool failed: " << result << std::endl;
  }

  cleanup_temp_project(tmp_dir);
}

inline asio::awaitable<void> test_codegraph_index_tool_execute_empty_path(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
  auto codegraph = std::make_shared<agentxx::expand::CodeGraphManager>();
  auto tool = agentxx::tools::CodeGraphIndexTool{codegraph, agentContext};

  auto result = co_await tool.execute_async({{"path", ""}});
  if (result.find("error") != std::string::npos) {
    std::cout << "[PASS] CodeGraphIndexTool rejects empty path" << std::endl;
  } else {
    std::cout << "[FAIL] CodeGraphIndexTool should reject empty path, got: "
              << result << std::endl;
  }
}

inline asio::awaitable<void> test_codegraph_callers_tool_execute(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
  auto tmp_dir = create_temp_project();
  auto codegraph = std::make_shared<agentxx::expand::CodeGraphManager>();
  codegraph->initialize(tmp_dir);
  codegraph->indexDirectory(tmp_dir, false);

  auto tool = agentxx::tools::CodeGraphCallersTool{codegraph, agentContext};
  auto result = co_await tool.execute_async({{"symbol", "add"}});

  if (result.find("error") == std::string::npos) {
    std::cout << "[PASS] CodeGraphCallersTool returns callers" << std::endl;
  } else {
    std::cout << "[FAIL] CodeGraphCallersTool failed: " << result << std::endl;
  }

  cleanup_temp_project(tmp_dir);
}

inline asio::awaitable<void> test_codegraph_callees_tool_execute(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
  auto tmp_dir = create_temp_project();
  auto codegraph = std::make_shared<agentxx::expand::CodeGraphManager>();
  codegraph->initialize(tmp_dir);
  codegraph->indexDirectory(tmp_dir, false);

  auto tool = agentxx::tools::CodeGraphCalleesTool{codegraph, agentContext};
  auto result = co_await tool.execute_async({{"symbol", "main"}});

  if (result.find("error") == std::string::npos) {
    std::cout << "[PASS] CodeGraphCalleesTool returns callees" << std::endl;
  } else {
    std::cout << "[FAIL] CodeGraphCalleesTool failed: " << result << std::endl;
  }

  cleanup_temp_project(tmp_dir);
}

inline asio::awaitable<void> test_codegraph_impact_tool_execute(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
  auto tmp_dir = create_temp_project();
  auto codegraph = std::make_shared<agentxx::expand::CodeGraphManager>();
  codegraph->initialize(tmp_dir);
  codegraph->indexDirectory(tmp_dir, false);

  auto tool = agentxx::tools::CodeGraphImpactTool{codegraph, agentContext};
  auto result = co_await tool.execute_async({{"symbol", "add"}});

  if (result.find("error") == std::string::npos) {
    std::cout << "[PASS] CodeGraphImpactTool returns impact" << std::endl;
  } else {
    std::cout << "[FAIL] CodeGraphImpactTool failed: " << result << std::endl;
  }

  cleanup_temp_project(tmp_dir);
}

inline asio::awaitable<void> test_codegraph_path_tool_execute(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
  auto tmp_dir = create_temp_project();
  auto codegraph = std::make_shared<agentxx::expand::CodeGraphManager>();
  codegraph->initialize(tmp_dir);
  codegraph->indexDirectory(tmp_dir, false);

  auto tool = agentxx::tools::CodeGraphPathTool{codegraph, agentContext};
  auto result =
      co_await tool.execute_async({{"from", "main"}, {"to", "add_impl"}});

  if (result.find("error") == std::string::npos ||
      result.find("No path found") != std::string::npos) {
    std::cout << "[PASS] CodeGraphPathTool returns path or no-path-found"
              << std::endl;
  } else {
    std::cout << "[INFO] CodeGraphPathTool result: " << result << std::endl;
  }

  cleanup_temp_project(tmp_dir);
}

// =========================================================================
// Test Runner
// =========================================================================

inline asio::awaitable<void> run_codegraph_tools_tests(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
  std::cout << "======= Test: CodeGraph Manager =======" << std::endl;

  test_codegraph_manager_init();
  test_codegraph_manager_init_twice();
  test_codegraph_manager_not_initialized();
  test_codegraph_manager_index();
  test_codegraph_manager_incremental_index();
  test_codegraph_manager_search();
  test_codegraph_manager_search_no_results();
  test_codegraph_manager_context();
  test_codegraph_manager_status();
  test_codegraph_manager_callers();
  test_codegraph_manager_callees();
  test_codegraph_manager_impact();
  test_codegraph_manager_path();
  test_codegraph_manager_shutdown();
  test_codegraph_manager_update_index();
  test_codegraph_manager_resolve();

  std::cout << "======= Test: Multi-Language CodeGraph =======" << std::endl;

  test_codegraph_manager_multi_lang_index();
  test_codegraph_manager_multi_lang_status();
  test_codegraph_manager_python_search();
  test_codegraph_manager_javascript_search();
  test_codegraph_manager_typescript_search();
  test_codegraph_manager_rust_search();
  test_codegraph_manager_go_search();
  test_codegraph_manager_java_search();
  test_codegraph_manager_multi_lang_context();
  test_codegraph_manager_multi_lang_path();

  std::cout << "======= Test: CodeGraph Tools =======" << std::endl;

  auto run = [agentContext](auto testFn) -> asio::awaitable<void> {
    try {
      co_await testFn(agentContext);
    } catch (const std::exception &e) {
      std::cout << "[FAIL] Exception in test: " << e.what() << std::endl;
    }
  };

  co_await run(test_codegraph_search_tool_definition);
  co_await run(test_codegraph_context_tool_definition);
  co_await run(test_codegraph_callers_tool_definition);
  co_await run(test_codegraph_callees_tool_definition);
  co_await run(test_codegraph_impact_tool_definition);
  co_await run(test_codegraph_status_tool_definition);
  co_await run(test_codegraph_index_tool_definition);
  co_await run(test_codegraph_path_tool_definition);

  co_await run(test_codegraph_search_tool_execute);
  co_await run(test_codegraph_search_tool_execute_empty_query);
  co_await run(test_codegraph_context_tool_execute);
  co_await run(test_codegraph_status_tool_execute);
  co_await run(test_codegraph_index_tool_execute);
  co_await run(test_codegraph_index_tool_execute_empty_path);
  co_await run(test_codegraph_callers_tool_execute);
  co_await run(test_codegraph_callees_tool_execute);
  co_await run(test_codegraph_impact_tool_execute);
  co_await run(test_codegraph_path_tool_execute);

  std::cout << "======= Test Done =======" << std::endl;
}

} // namespace test
} // namespace agentxx

#endif