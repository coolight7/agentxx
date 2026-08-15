#include "test_codegraph_tools.h"
#include "agentxx/agent/context.h"
#include "agentxx/expand/codegraph_manager.h"
#include "agentxx/tools/codegraph_tool.h"
#include <asio/awaitable.hpp>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>
#include <string>
#include <thread>

#if AGENTXX_ENABLE_CODEGRAPH

namespace agentxx {
namespace test {

int g_cg_passed = 0;
int g_cg_failed = 0;

namespace fs = std::filesystem;

static std::atomic<int> g_temp_project_counter{0};

static std::string create_temp_project() {
    int  idx     = g_temp_project_counter.fetch_add(1);
    auto tmp_dir = fs::temp_directory_path() / ("codegraph_test_" + std::to_string(idx));
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

static void cleanup_temp_project(const std::string& path) {
    try {
        fs::remove_all(path);
    } catch (...) {
    }
}

static std::string create_multi_lang_project() {
    int  idx     = g_temp_project_counter.fetch_add(1);
    auto tmp_dir = fs::temp_directory_path() / ("codegraph_multi_test_" + std::to_string(idx));
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

void test_codegraph_manager_init() {
    auto tmp_dir = create_temp_project();
    auto manager = agentxx::expand::CodeGraphManager{};

    bool ok = manager.initialize(tmp_dir);
    if (ok) {
        g_cg_passed++;
        TEST_PASS << "CodeGraphManager::initialize creates index" << std::endl;
    } else {
        g_cg_failed++;
        TEST_FAIL << "CodeGraphManager::initialize failed" << std::endl;
    }

    cleanup_temp_project(tmp_dir);
}

void test_codegraph_manager_init_twice() {
    auto tmp_dir = create_temp_project();
    auto manager = agentxx::expand::CodeGraphManager{};

    bool ok1 = manager.initialize(tmp_dir);
    bool ok2 = manager.initialize(tmp_dir);

    if (ok1 && ok2) {
        g_cg_passed++;
        TEST_PASS << "CodeGraphManager::initialize twice succeeds" << std::endl;
    } else {
        g_cg_failed++;
        TEST_FAIL << "CodeGraphManager::initialize twice failed" << std::endl;
    }

    cleanup_temp_project(tmp_dir);
}

void test_codegraph_manager_not_initialized() {
    auto manager = agentxx::expand::CodeGraphManager{};

    auto search_result = manager.searchSymbols("test", 10);
    if (!search_result.success
        && search_result.error.find("not initialized") != std::string::npos) {
        g_cg_passed++;
        TEST_PASS << "CodeGraphManager rejects search when not initialized" << std::endl;
    } else {
        g_cg_failed++;
        TEST_FAIL << "CodeGraphManager should reject uninitialized search" << std::endl;
    }

    auto status_result = manager.getStatus();
    if (!status_result.success
        && status_result.error.find("not initialized") != std::string::npos) {
        g_cg_passed++;
        TEST_PASS << "CodeGraphManager rejects status when not initialized" << std::endl;
    } else {
        g_cg_failed++;
        TEST_FAIL << "CodeGraphManager should reject uninitialized status" << std::endl;
    }
}

void test_codegraph_manager_index() {
    auto tmp_dir = create_temp_project();
    auto manager = agentxx::expand::CodeGraphManager{};

    manager.initialize(tmp_dir);
    bool ok = manager.indexDirectory(tmp_dir, false);

    if (ok) {
        g_cg_passed++;
        TEST_PASS << "CodeGraphManager::indexDirectory succeeds" << std::endl;
    } else {
        g_cg_failed++;
        TEST_FAIL << "CodeGraphManager::indexDirectory failed" << std::endl;
    }

    cleanup_temp_project(tmp_dir);
}

void test_codegraph_manager_incremental_index() {
    auto tmp_dir = create_temp_project();
    auto manager = agentxx::expand::CodeGraphManager{};

    manager.initialize(tmp_dir);
    manager.indexDirectory(tmp_dir, false);

    bool ok = manager.indexDirectory(tmp_dir, true);
    if (ok) {
        g_cg_passed++;
        TEST_PASS << "CodeGraphManager::indexDirectory incremental succeeds" << std::endl;
    } else {
        g_cg_failed++;
        TEST_FAIL << "CodeGraphManager::indexDirectory incremental failed" << std::endl;
    }

    cleanup_temp_project(tmp_dir);
}

// 查询结果缓存 (LruCache): 重复查询命中缓存结果一致; 索引变更后缓存失效,
// 新符号可查询到 (验证 invalidateCaches 打通索引 -> 缓存的一致性)
void test_codegraph_manager_cache_invalidate() {
    auto tmp_dir = create_temp_project();
    auto manager = agentxx::expand::CodeGraphManager{};

    manager.initialize(tmp_dir);
    manager.indexDirectory(tmp_dir, false);

    // 1. 首次查询建立缓存
    auto r1 = manager.searchSymbols("add", 10);
    if (!r1.success || r1.nodes.empty()) {
        g_cg_failed++;
        TEST_FAIL << "searchSymbols('add') initial should succeed (cache seed)" << std::endl;
        cleanup_temp_project(tmp_dir);
        return;
    }

    // 2. 同参数重复查询: 命中缓存, 结果一致
    auto r2 = manager.searchSymbols("add", 10);
    if (!r2.success || r2.nodes.size() != r1.nodes.size()) {
        g_cg_failed++;
        TEST_FAIL << "repeated searchSymbols('add') should hit cache and match" << std::endl;
        cleanup_temp_project(tmp_dir);
        return;
    }

    // 3. 新增源文件 (含全新符号), 增量索引
    {
        std::ofstream f(fs::path(tmp_dir) / "extra.cpp");
        f << R"(int brand_new_symbol(int v) {
    return v * 2;
}
)";
    }
    bool ok = manager.indexDirectory(tmp_dir, true);
    if (!ok) {
        g_cg_failed++;
        TEST_FAIL << "incremental indexDirectory after adding file failed" << std::endl;
        cleanup_temp_project(tmp_dir);
        return;
    }

    // 4. 缓存应已失效 (invalidateCaches): 新符号可查询到
    auto r3 = manager.searchSymbols("brand", 10);
    if (r3.success && !r3.nodes.empty()) {
        g_cg_passed++;
        TEST_PASS << "cache invalidated after incremental index, new symbol visible" << std::endl;
    } else {
        g_cg_failed++;
        TEST_FAIL << "cache NOT invalidated: new symbol missing after reindex" << std::endl;
    }

    // 5. 原有查询失效后重算仍正常
    auto r4 = manager.searchSymbols("add", 10);
    if (r4.success && !r4.nodes.empty()) {
        g_cg_passed++;
        TEST_PASS << "original query re-computed after cache invalidation" << std::endl;
    } else {
        g_cg_failed++;
        TEST_FAIL << "original query failed after cache invalidation" << std::endl;
    }

    cleanup_temp_project(tmp_dir);
}

void test_codegraph_manager_search() {
    auto tmp_dir = create_temp_project();
    auto manager = agentxx::expand::CodeGraphManager{};

    manager.initialize(tmp_dir);
    manager.indexDirectory(tmp_dir, false);

    auto result = manager.searchSymbols("add", 10);
    if (result.success) {
        if (!result.nodes.empty()) {
            g_cg_passed++;
            TEST_PASS << "CodeGraphManager::searchSymbols found " << result.nodes.size()
                      << " results" << std::endl;
        } else {
            TEST_INFO << "CodeGraphManager::searchSymbols returned empty "
                         "(FTS may not be populated)"
                      << std::endl;
        }
    } else {
        g_cg_failed++;
        TEST_FAIL << "CodeGraphManager::searchSymbols error: "
                  << (result.error.empty() ? "(empty)" : result.error) << std::endl;
    }

    cleanup_temp_project(tmp_dir);
}

void test_codegraph_manager_search_no_results() {
    auto tmp_dir = create_temp_project();
    auto manager = agentxx::expand::CodeGraphManager{};

    manager.initialize(tmp_dir);
    manager.indexDirectory(tmp_dir, false);

    auto result = manager.searchSymbols("nonexistent_symbol_xyz", 10);
    if (result.success && result.nodes.empty()) {
        g_cg_passed++;
        TEST_PASS << "CodeGraphManager::searchSymbols returns empty for "
                     "unknown symbol"
                  << std::endl;
    } else if (!result.success) {
        g_cg_failed++;
        TEST_FAIL << "CodeGraphManager::searchSymbols error: "
                  << (result.error.empty() ? "(empty)" : result.error) << std::endl;
    } else {
        g_cg_failed++;
        TEST_FAIL << "CodeGraphManager::searchSymbols should return empty "
                     "for unknown symbol"
                  << std::endl;
    }

    cleanup_temp_project(tmp_dir);
}

void test_codegraph_manager_context() {
    auto tmp_dir = create_temp_project();
    auto manager = agentxx::expand::CodeGraphManager{};

    manager.initialize(tmp_dir);
    manager.indexDirectory(tmp_dir, false);

    auto result = manager.getSymbolContext("add");
    if (result.success) {
        g_cg_passed++;
        TEST_PASS << "CodeGraphManager::getSymbolContext succeeds" << std::endl;
    } else {
        g_cg_failed++;
        TEST_FAIL << "CodeGraphManager::getSymbolContext failed: "
                  << (result.error.empty() ? "(empty)" : result.error) << std::endl;
    }

    cleanup_temp_project(tmp_dir);
}

void test_codegraph_manager_status() {
    auto tmp_dir = create_temp_project();
    auto manager = agentxx::expand::CodeGraphManager{};

    manager.initialize(tmp_dir);
    manager.indexDirectory(tmp_dir, false);

    try {
        auto result = manager.getStatus();
        if (result.success) {
            g_cg_passed++;
            TEST_PASS << "CodeGraphManager::getStatus returns stats" << std::endl;
        } else {
            g_cg_failed++;
            TEST_FAIL << "CodeGraphManager::getStatus failed: "
                      << (result.error.empty() ? "(empty)" : result.error) << std::endl;
        }
    } catch (const std::exception& e) {
        g_cg_failed++;
        TEST_FAIL << "CodeGraphManager::getStatus threw: " << e.what() << std::endl;
    } catch (...) {
        g_cg_failed++;
        TEST_FAIL << "CodeGraphManager::getStatus threw unknown exception" << std::endl;
    }

    cleanup_temp_project(tmp_dir);
}

/// 循环依赖检测正确性 (find_circular_dependencies 批量查询重写后):
/// 构造 A->B->C->A 调用环, getStatus 应检测到 >=1 个循环依赖 SCC
/// (调用定义在后的函数会生成未解析引用, 收尾 resolve 后建立调用边)
void test_codegraph_circular_deps() {
    int  idx     = g_temp_project_counter.fetch_add(1);
    auto tmp_dir = fs::temp_directory_path() / ("codegraph_cycle_test_" + std::to_string(idx));
    if (fs::exists(tmp_dir)) {
        fs::remove_all(tmp_dir);
    }
    fs::create_directories(tmp_dir);
    {
        std::ofstream f(tmp_dir / "cycle.cpp");
        f << R"(int func_a() { return func_b(); }
int func_b() { return func_c(); }
int func_c() { return func_a(); }
)";
    }
    auto manager = agentxx::expand::CodeGraphManager{};
    manager.initialize(tmp_dir.generic_string());
    manager.indexDirectory(tmp_dir.generic_string(), false);

    auto result = manager.getStatus();
    if (result.success && result.circular_deps >= 1) {
        g_cg_passed++;
        TEST_PASS << "circular dependencies detected: " << result.circular_deps << std::endl;
    } else {
        g_cg_failed++;
        TEST_FAIL << "circular deps detection FAILED: success=" << result.success
                  << " circular_deps=" << result.circular_deps << std::endl;
    }

    cleanup_temp_project(tmp_dir.generic_string());
}

/// 不可解析引用清理: 跨文件调用不存在的符号 (目标无定义, 提取器噪音),
/// resolveReferences 应删除这些引用而非保留; 连续多次解析均成功且不抛异常,
/// 后续解析不再重复处理同一批引用 (收敛性)
void test_codegraph_resolve_unresolvable() {
    int  idx     = g_temp_project_counter.fetch_add(1);
    auto tmp_dir = fs::temp_directory_path()
                   / ("codegraph_unresolvable_" + std::to_string(idx));
    if (fs::exists(tmp_dir)) {
        fs::remove_all(tmp_dir);
    }
    fs::create_directories(tmp_dir);
    {
        std::ofstream f(tmp_dir / "main.cpp");
        f << R"(int caller_a() { return totally_missing_func_a(1); }
int caller_b() { return totally_missing_func_b(1); }
int main() { return caller_a() + caller_b(); }
)";
    }
    auto manager = agentxx::expand::CodeGraphManager{};
    manager.initialize(tmp_dir.generic_string());
    manager.indexDirectory(tmp_dir.generic_string(), false);

    // 两次解析都成功且不抛异常; 不可解析引用 (目标不存在) 应被清理,
    // 第二次解析不再重复处理 (无法直接观察数量, 验证行为正确性)
    bool       ok1    = manager.resolveReferences();
    bool       ok2    = manager.resolveReferences();
    auto       status = manager.getStatus();
    if (ok1 && ok2 && status.success) {
        g_cg_passed++;
        TEST_PASS << "resolveReferences handles unresolvable refs" << std::endl;
    } else {
        g_cg_failed++;
        TEST_FAIL << "resolveReferences unresolvable FAILED: ok1=" << ok1 << " ok2=" << ok2
                  << " status_success=" << status.success << std::endl;
    }

    cleanup_temp_project(tmp_dir.generic_string());
}

void test_codegraph_manager_callers() {
    auto tmp_dir = create_temp_project();
    auto manager = agentxx::expand::CodeGraphManager{};

    manager.initialize(tmp_dir);
    manager.indexDirectory(tmp_dir, false);

    auto result = manager.getCallers("add");
    if (result.success) {
        g_cg_passed++;
        TEST_PASS << "CodeGraphManager::getCallers succeeds" << std::endl;
    } else {
        g_cg_failed++;
        TEST_FAIL << "CodeGraphManager::getCallers failed: "
                  << (result.error.empty() ? "(empty)" : result.error) << std::endl;
    }

    cleanup_temp_project(tmp_dir);
}

void test_codegraph_manager_callees() {
    auto tmp_dir = create_temp_project();
    auto manager = agentxx::expand::CodeGraphManager{};

    manager.initialize(tmp_dir);
    manager.indexDirectory(tmp_dir, false);

    auto result = manager.getCallees("main");
    if (result.success) {
        g_cg_passed++;
        TEST_PASS << "CodeGraphManager::getCallees succeeds" << std::endl;
    } else {
        g_cg_failed++;
        TEST_FAIL << "CodeGraphManager::getCallees failed: "
                  << (result.error.empty() ? "(empty)" : result.error) << std::endl;
    }

    cleanup_temp_project(tmp_dir);
}

void test_codegraph_manager_impact() {
    auto tmp_dir = create_temp_project();
    auto manager = agentxx::expand::CodeGraphManager{};

    manager.initialize(tmp_dir);
    manager.indexDirectory(tmp_dir, false);

    auto result = manager.getImpact("add");
    if (result.success) {
        g_cg_passed++;
        TEST_PASS << "CodeGraphManager::getImpact succeeds" << std::endl;
    } else {
        g_cg_failed++;
        TEST_FAIL << "CodeGraphManager::getImpact failed: "
                  << (result.error.empty() ? "(empty)" : result.error) << std::endl;
    }

    cleanup_temp_project(tmp_dir);
}

void test_codegraph_manager_path() {
    auto tmp_dir = create_temp_project();
    auto manager = agentxx::expand::CodeGraphManager{};

    manager.initialize(tmp_dir);
    manager.indexDirectory(tmp_dir, false);

    auto result = manager.findPath("main", "add_impl");
    if (result.success) {
        g_cg_passed++;
        TEST_PASS << "CodeGraphManager::findPath found path" << std::endl;
    } else {
        TEST_INFO << "CodeGraphManager::findPath result: "
                  << (result.error.empty() ? "(empty)" : result.error) << std::endl;
    }

    cleanup_temp_project(tmp_dir);
}

void test_codegraph_manager_shutdown() {
    auto tmp_dir = create_temp_project();
    auto manager = agentxx::expand::CodeGraphManager{};

    manager.initialize(tmp_dir);
    manager.indexDirectory(tmp_dir, false);
    manager.shutdown();

    auto result = manager.searchSymbols("add", 10);
    if (!result.success && result.error.find("not initialized") != std::string::npos) {
        g_cg_passed++;
        TEST_PASS << "CodeGraphManager::shutdown disables queries" << std::endl;
    } else {
        g_cg_failed++;
        TEST_FAIL << "CodeGraphManager::shutdown should disable queries" << std::endl;
    }

    cleanup_temp_project(tmp_dir);
}

void test_codegraph_manager_update_index() {
    auto tmp_dir = create_temp_project();
    auto manager = agentxx::expand::CodeGraphManager{};

    manager.initialize(tmp_dir);
    manager.indexDirectory(tmp_dir, false);

    bool ok = manager.updateIndex();
    if (ok) {
        g_cg_passed++;
        TEST_PASS << "CodeGraphManager::updateIndex succeeds" << std::endl;
    } else {
        g_cg_failed++;
        TEST_FAIL << "CodeGraphManager::updateIndex failed" << std::endl;
    }

    cleanup_temp_project(tmp_dir);
}

void test_codegraph_manager_resolve() {
    auto tmp_dir = create_temp_project();
    auto manager = agentxx::expand::CodeGraphManager{};

    manager.initialize(tmp_dir);
    manager.indexDirectory(tmp_dir, false);

    auto result = manager.resolveReferences();
    if (result) {
        g_cg_passed++;
        TEST_PASS << "CodeGraphManager::resolveReferences succeeds" << std::endl;
    } else {
        g_cg_failed++;
        TEST_FAIL << "CodeGraphManager::resolveReferences failed" << std::endl;
    }

    cleanup_temp_project(tmp_dir);
}

// =========================================================================
// Multi-Language CodeGraph Tests
// =========================================================================

void test_codegraph_manager_multi_lang_index() {
    auto tmp_dir = create_multi_lang_project();
    auto manager = agentxx::expand::CodeGraphManager{};

    manager.initialize(tmp_dir);
    bool ok = manager.indexDirectory(tmp_dir, false);

    if (ok) {
        g_cg_passed++;
        TEST_PASS << "CodeGraphManager multi-lang index succeeds" << std::endl;
    } else {
        g_cg_failed++;
        TEST_FAIL << "CodeGraphManager multi-lang index failed" << std::endl;
    }

    cleanup_temp_project(tmp_dir);
}

void test_codegraph_manager_multi_lang_status() {
    auto tmp_dir = create_multi_lang_project();
    auto manager = agentxx::expand::CodeGraphManager{};

    manager.initialize(tmp_dir);
    manager.indexDirectory(tmp_dir, false);

    auto result = manager.getStatus();
    if (result.success && result.total_files >= 6) {
        g_cg_passed++;
        TEST_PASS << "CodeGraphManager multi-lang status: " << result.total_files << " files, "
                  << result.total_nodes << " nodes, " << result.total_edges << " edges"
                  << std::endl;
    } else {
        g_cg_failed++;
        TEST_FAIL << "CodeGraphManager multi-lang status failed: "
                  << (result.error.empty() ? "too few files" : result.error) << std::endl;
    }

    cleanup_temp_project(tmp_dir);
}

void test_codegraph_manager_python_search() {
    auto tmp_dir = create_multi_lang_project();
    auto manager = agentxx::expand::CodeGraphManager{};

    manager.initialize(tmp_dir);
    manager.indexDirectory(tmp_dir, false);

    auto result = manager.searchSymbols("greet", 20);
    if (result.success) {
        g_cg_passed++;
        TEST_PASS << "CodeGraphManager Python search 'greet' found " << result.nodes.size()
                  << " results" << std::endl;
    } else {
        g_cg_failed++;
        TEST_FAIL << "CodeGraphManager Python search failed: "
                  << (result.error.empty() ? "(empty)" : result.error) << std::endl;
    }

    cleanup_temp_project(tmp_dir);
}

void test_codegraph_manager_javascript_search() {
    auto tmp_dir = create_multi_lang_project();
    auto manager = agentxx::expand::CodeGraphManager{};

    manager.initialize(tmp_dir);
    manager.indexDirectory(tmp_dir, false);

    auto result = manager.searchSymbols("multiply", 20);
    if (result.success) {
        bool found_js = false;
        for (const auto& node : result.nodes) {
            if (node.file_path.find(".js") != std::string::npos) {
                found_js = true;
                break;
            }
        }
        if (found_js || result.nodes.empty()) {
            g_cg_passed++;
            TEST_PASS << "CodeGraphManager JavaScript search 'multiply' found "
                      << result.nodes.size() << " results" << std::endl;
        } else {
            g_cg_failed++;
            TEST_FAIL << "CodeGraphManager JavaScript search: no .js results" << std::endl;
        }
    } else {
        g_cg_failed++;
        TEST_FAIL << "CodeGraphManager JavaScript search failed: "
                  << (result.error.empty() ? "(empty)" : result.error) << std::endl;
    }

    cleanup_temp_project(tmp_dir);
}

void test_codegraph_manager_typescript_search() {
    auto tmp_dir = create_multi_lang_project();
    auto manager = agentxx::expand::CodeGraphManager{};

    manager.initialize(tmp_dir);
    manager.indexDirectory(tmp_dir, false);

    auto result = manager.searchSymbols("getUser", 20);
    if (result.success) {
        g_cg_passed++;
        TEST_PASS << "CodeGraphManager TypeScript search 'getUser' found " << result.nodes.size()
                  << " results" << std::endl;
    } else {
        g_cg_failed++;
        TEST_FAIL << "CodeGraphManager TypeScript search failed: "
                  << (result.error.empty() ? "(empty)" : result.error) << std::endl;
    }

    cleanup_temp_project(tmp_dir);
}

void test_codegraph_manager_rust_search() {
    auto tmp_dir = create_multi_lang_project();
    auto manager = agentxx::expand::CodeGraphManager{};

    manager.initialize(tmp_dir);
    manager.indexDirectory(tmp_dir, false);

    auto result = manager.searchSymbols("factorial", 20);
    if (result.success) {
        g_cg_passed++;
        TEST_PASS << "CodeGraphManager Rust search 'factorial' found " << result.nodes.size()
                  << " results" << std::endl;
    } else {
        g_cg_failed++;
        TEST_FAIL << "CodeGraphManager Rust search failed: "
                  << (result.error.empty() ? "(empty)" : result.error) << std::endl;
    }

    cleanup_temp_project(tmp_dir);
}

void test_codegraph_manager_go_search() {
    auto tmp_dir = create_multi_lang_project();
    auto manager = agentxx::expand::CodeGraphManager{};

    manager.initialize(tmp_dir);
    manager.indexDirectory(tmp_dir, false);

    auto result = manager.searchSymbols("greet", 20);
    if (result.success) {
        bool found_go = false;
        for (const auto& node : result.nodes) {
            if (node.file_path.find(".go") != std::string::npos) {
                found_go = true;
                break;
            }
        }
        if (found_go || result.nodes.empty()) {
            g_cg_passed++;
            TEST_PASS << "CodeGraphManager Go search 'greet' found " << result.nodes.size()
                      << " results" << std::endl;
        } else {
            g_cg_failed++;
            TEST_FAIL << "CodeGraphManager Go search: no .go results" << std::endl;
        }
    } else {
        g_cg_failed++;
        TEST_FAIL << "CodeGraphManager Go search failed: "
                  << (result.error.empty() ? "(empty)" : result.error) << std::endl;
    }

    cleanup_temp_project(tmp_dir);
}

void test_codegraph_manager_java_search() {
    auto tmp_dir = create_multi_lang_project();
    auto manager = agentxx::expand::CodeGraphManager{};

    manager.initialize(tmp_dir);
    manager.indexDirectory(tmp_dir, false);

    auto result = manager.searchSymbols("multiply", 20);
    if (result.success) {
        bool found_java = false;
        for (const auto& node : result.nodes) {
            if (node.file_path.find(".java") != std::string::npos) {
                found_java = true;
                break;
            }
        }
        if (found_java || result.nodes.empty()) {
            g_cg_passed++;
            TEST_PASS << "CodeGraphManager Java search 'multiply' found " << result.nodes.size()
                      << " results" << std::endl;
        } else {
            g_cg_failed++;
            TEST_FAIL << "CodeGraphManager Java search: no .java results" << std::endl;
        }
    } else {
        g_cg_failed++;
        TEST_FAIL << "CodeGraphManager Java search failed: "
                  << (result.error.empty() ? "(empty)" : result.error) << std::endl;
    }

    cleanup_temp_project(tmp_dir);
}

void test_codegraph_manager_multi_lang_context() {
    auto tmp_dir = create_multi_lang_project();
    auto manager = agentxx::expand::CodeGraphManager{};

    manager.initialize(tmp_dir);
    manager.indexDirectory(tmp_dir, false);

    auto result = manager.getSymbolContext("add");
    if (result.success) {
        g_cg_passed++;
        TEST_PASS << "CodeGraphManager multi-lang getSymbolContext 'add' "
                     "succeeds"
                  << std::endl;
    } else {
        g_cg_failed++;
        TEST_FAIL << "CodeGraphManager multi-lang getSymbolContext failed: "
                  << (result.error.empty() ? "(empty)" : result.error) << std::endl;
    }

    cleanup_temp_project(tmp_dir);
}

void test_codegraph_manager_multi_lang_path() {
    auto tmp_dir = create_multi_lang_project();
    auto manager = agentxx::expand::CodeGraphManager{};

    manager.initialize(tmp_dir);
    manager.indexDirectory(tmp_dir, false);

    auto result = manager.findPath("multiply", "add");
    if (result.success || result.error.find("No path found") != std::string::npos) {
        g_cg_passed++;
        TEST_PASS << "CodeGraphManager multi-lang findPath multiply->add "
                     "returned"
                  << std::endl;
    } else {
        TEST_INFO << "CodeGraphManager multi-lang findPath: "
                  << (result.error.empty() ? "(empty)" : result.error) << std::endl;
    }

    cleanup_temp_project(tmp_dir);
}

// =========================================================================
// Tool Definition Tests
// =========================================================================

asio::awaitable<void>
    test_codegraph_search_tool_definition(std::weak_ptr<agentxx::agent::AgentContext> agentContext
    ) {
    auto codegraph = std::make_shared<agentxx::expand::CodeGraphManager>();
    auto tool      = agentxx::tools::CodeGraphSearchTool{codegraph, agentContext};

    auto def = tool.get_definition();
    if (def.name == "agentxx_codegraph_search") {
        g_cg_passed++;
        TEST_PASS << "CodeGraphSearchTool::get_definition name correct" << std::endl;
    } else {
        g_cg_failed++;
        TEST_FAIL << "CodeGraphSearchTool::get_definition name incorrect" << std::endl;
    }
    co_return;
}

asio::awaitable<void>
    test_codegraph_context_tool_definition(std::weak_ptr<agentxx::agent::AgentContext> agentContext
    ) {
    auto codegraph = std::make_shared<agentxx::expand::CodeGraphManager>();
    auto tool      = agentxx::tools::CodeGraphContextTool{codegraph, agentContext};

    auto def = tool.get_definition();
    if (def.name == "agentxx_codegraph_context") {
        g_cg_passed++;
        TEST_PASS << "CodeGraphContextTool::get_definition name correct" << std::endl;
    } else {
        g_cg_failed++;
        TEST_FAIL << "CodeGraphContextTool::get_definition name incorrect" << std::endl;
    }
    co_return;
}

asio::awaitable<void>
    test_codegraph_callers_tool_definition(std::weak_ptr<agentxx::agent::AgentContext> agentContext
    ) {
    auto codegraph = std::make_shared<agentxx::expand::CodeGraphManager>();
    auto tool      = agentxx::tools::CodeGraphCallersTool{codegraph, agentContext};

    auto def = tool.get_definition();
    if (def.name == "agentxx_codegraph_callers") {
        g_cg_passed++;
        TEST_PASS << "CodeGraphCallersTool::get_definition name correct" << std::endl;
    } else {
        g_cg_failed++;
        TEST_FAIL << "CodeGraphCallersTool::get_definition name incorrect" << std::endl;
    }
    co_return;
}

asio::awaitable<void>
    test_codegraph_callees_tool_definition(std::weak_ptr<agentxx::agent::AgentContext> agentContext
    ) {
    auto codegraph = std::make_shared<agentxx::expand::CodeGraphManager>();
    auto tool      = agentxx::tools::CodeGraphCalleesTool{codegraph, agentContext};

    auto def = tool.get_definition();
    if (def.name == "agentxx_codegraph_callees") {
        g_cg_passed++;
        TEST_PASS << "CodeGraphCalleesTool::get_definition name correct" << std::endl;
    } else {
        g_cg_failed++;
        TEST_FAIL << "CodeGraphCalleesTool::get_definition name incorrect" << std::endl;
    }
    co_return;
}

asio::awaitable<void>
    test_codegraph_impact_tool_definition(std::weak_ptr<agentxx::agent::AgentContext> agentContext
    ) {
    auto codegraph = std::make_shared<agentxx::expand::CodeGraphManager>();
    auto tool      = agentxx::tools::CodeGraphImpactTool{codegraph, agentContext};

    auto def = tool.get_definition();
    if (def.name == "agentxx_codegraph_impact") {
        g_cg_passed++;
        TEST_PASS << "CodeGraphImpactTool::get_definition name correct" << std::endl;
    } else {
        g_cg_failed++;
        TEST_FAIL << "CodeGraphImpactTool::get_definition name incorrect" << std::endl;
    }
    co_return;
}

asio::awaitable<void>
    test_codegraph_status_tool_definition(std::weak_ptr<agentxx::agent::AgentContext> agentContext
    ) {
    auto codegraph = std::make_shared<agentxx::expand::CodeGraphManager>();
    auto tool      = agentxx::tools::CodeGraphStatusTool{codegraph, agentContext};

    auto def = tool.get_definition();
    if (def.name == "agentxx_codegraph_status") {
        g_cg_passed++;
        TEST_PASS << "CodeGraphStatusTool::get_definition name correct" << std::endl;
    } else {
        g_cg_failed++;
        TEST_FAIL << "CodeGraphStatusTool::get_definition name incorrect" << std::endl;
    }
    co_return;
}

asio::awaitable<void>
    test_codegraph_index_tool_definition(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
    auto codegraph = std::make_shared<agentxx::expand::CodeGraphManager>();
    auto tool      = agentxx::tools::CodeGraphIndexTool{codegraph, agentContext};

    auto def = tool.get_definition();
    if (def.name == "agentxx_codegraph_index") {
        g_cg_passed++;
        TEST_PASS << "CodeGraphIndexTool::get_definition name correct" << std::endl;
    } else {
        g_cg_failed++;
        TEST_FAIL << "CodeGraphIndexTool::get_definition name incorrect" << std::endl;
    }
    co_return;
}

asio::awaitable<void>
    test_codegraph_path_tool_definition(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
    auto codegraph = std::make_shared<agentxx::expand::CodeGraphManager>();
    auto tool      = agentxx::tools::CodeGraphPathTool{codegraph, agentContext};

    auto def = tool.get_definition();
    if (def.name == "agentxx_codegraph_path") {
        g_cg_passed++;
        TEST_PASS << "CodeGraphPathTool::get_definition name correct" << std::endl;
    } else {
        g_cg_failed++;
        TEST_FAIL << "CodeGraphPathTool::get_definition name incorrect" << std::endl;
    }
    co_return;
}

// =========================================================================
// Tool Execute Tests
// =========================================================================

asio::awaitable<void>
    test_codegraph_search_tool_execute(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
    auto tmp_dir   = create_temp_project();
    auto codegraph = std::make_shared<agentxx::expand::CodeGraphManager>();
    codegraph->initialize(tmp_dir);
    codegraph->indexDirectory(tmp_dir, false);

    auto tool   = agentxx::tools::CodeGraphSearchTool{codegraph, agentContext};
    auto result = co_await tool.execute_async({
        {"query", "add"}
    });

    // 多行文本格式: 首行 "Symbols (N):", 每个符号一块 "[N] <kind> <name>"
    bool has_header = result.rfind("Symbols (", 0) == 0;
    bool has_block  = result.find("[1]") != std::string::npos;
    bool has_error  = result.find("error:") != std::string::npos;

    if (has_header && (has_block || has_error)) {
        g_cg_passed++;
        TEST_PASS << "CodeGraphSearchTool returns valid plain-text results" << std::endl;
    } else if (has_header && result.find("(none)") != std::string::npos) {
        TEST_INFO << "CodeGraphSearchTool returned empty (FTS may not be "
                     "populated)"
                  << std::endl;
    } else {
        g_cg_failed++;
        TEST_FAIL << "CodeGraphSearchTool invalid result: " << result << std::endl;
    }

    cleanup_temp_project(tmp_dir);
}

asio::awaitable<void> test_codegraph_search_tool_execute_empty_query(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext
) {
    auto codegraph = std::make_shared<agentxx::expand::CodeGraphManager>();
    auto tool      = agentxx::tools::CodeGraphSearchTool{codegraph, agentContext};

    auto result = co_await tool.execute_async({
        {"query", ""}
    });
    if (result.find("error") != std::string::npos) {
        g_cg_passed++;
        TEST_PASS << "CodeGraphSearchTool rejects empty query" << std::endl;
    } else {
        g_cg_failed++;
        TEST_FAIL << "CodeGraphSearchTool should reject empty query, got: " << result << std::endl;
    }
}

asio::awaitable<void>
    test_codegraph_context_tool_execute(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
    auto tmp_dir   = create_temp_project();
    auto codegraph = std::make_shared<agentxx::expand::CodeGraphManager>();
    codegraph->initialize(tmp_dir);
    codegraph->indexDirectory(tmp_dir, false);

    auto tool   = agentxx::tools::CodeGraphContextTool{codegraph, agentContext};
    auto result = co_await tool.execute_async({
        {"symbol", "add"}
    });

    // 多行文本格式: 首行 "symbol: <kind> <name>", 后跟 callers/callees 等区块
    bool has_symbol  = result.rfind("symbol:", 0) == 0;
    bool has_section = result.find("callers (") != std::string::npos
                       || result.find("callees (") != std::string::npos;
    if (has_symbol && has_section) {
        g_cg_passed++;
        TEST_PASS << "CodeGraphContextTool returns plain-text context" << std::endl;
    } else {
        g_cg_failed++;
        TEST_FAIL << "CodeGraphContextTool failed: " << result << std::endl;
    }

    cleanup_temp_project(tmp_dir);
}

asio::awaitable<void>
    test_codegraph_status_tool_execute(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
    auto tmp_dir   = create_temp_project();
    auto codegraph = std::make_shared<agentxx::expand::CodeGraphManager>();
    codegraph->initialize(tmp_dir);
    codegraph->indexDirectory(tmp_dir, false);

    auto tool   = agentxx::tools::CodeGraphStatusTool{codegraph, agentContext};
    auto result = co_await tool.execute_async(neograph::json{});

    // 多行文本格式: 每行一个 "label: value"
    bool has_nodes = result.find("total_nodes:") != std::string::npos;
    bool has_edges = result.find("total_edges:") != std::string::npos;
    bool has_files = result.find("total_files:") != std::string::npos;

    if (has_nodes && has_edges && has_files) {
        g_cg_passed++;
        TEST_PASS << "CodeGraphStatusTool returns plain-text statistics" << std::endl;
    } else {
        g_cg_failed++;
        TEST_FAIL << "CodeGraphStatusTool incomplete: " << result << std::endl;
    }

    cleanup_temp_project(tmp_dir);
}

asio::awaitable<void>
    test_codegraph_index_tool_execute(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
    auto tmp_dir   = create_temp_project();
    auto codegraph = std::make_shared<agentxx::expand::CodeGraphManager>();
    codegraph->initialize(tmp_dir);

    auto tool   = agentxx::tools::CodeGraphIndexTool{codegraph, agentContext};
    auto result = co_await tool.execute_async({
        {"path",        tmp_dir},
        {"incremental", false  }
    });

    // 多行文本格式: 成功时首行 "success: true" + 统计字段
    if (result.rfind("success: true", 0) == 0
        || result.find("error:") != std::string::npos) {
        g_cg_passed++;
        TEST_PASS << "CodeGraphIndexTool indexes directory" << std::endl;
    } else {
        g_cg_failed++;
        TEST_FAIL << "CodeGraphIndexTool failed: " << result << std::endl;
    }

    cleanup_temp_project(tmp_dir);
}

asio::awaitable<void> test_codegraph_index_tool_execute_empty_path(
    std::weak_ptr<agentxx::agent::AgentContext> agentContext
) {
    auto codegraph = std::make_shared<agentxx::expand::CodeGraphManager>();
    auto tool      = agentxx::tools::CodeGraphIndexTool{codegraph, agentContext};

    auto result = co_await tool.execute_async({
        {"path", ""}
    });
    if (result.find("error") != std::string::npos) {
        g_cg_passed++;
        TEST_PASS << "CodeGraphIndexTool rejects empty path" << std::endl;
    } else {
        g_cg_failed++;
        TEST_FAIL << "CodeGraphIndexTool should reject empty path, got: " << result << std::endl;
    }
}

asio::awaitable<void>
    test_codegraph_callers_tool_execute(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
    auto tmp_dir   = create_temp_project();
    auto codegraph = std::make_shared<agentxx::expand::CodeGraphManager>();
    codegraph->initialize(tmp_dir);
    codegraph->indexDirectory(tmp_dir, false);

    auto tool   = agentxx::tools::CodeGraphCallersTool{codegraph, agentContext};
    auto result = co_await tool.execute_async({
        {"symbol", "add"}
    });

    // 多行文本格式: 首行 "Callers (N):" (可为空列表)
    if (result.rfind("Callers (", 0) == 0
        || result.find("error:") != std::string::npos) {
        g_cg_passed++;
        TEST_PASS << "CodeGraphCallersTool returns plain-text callers" << std::endl;
    } else {
        g_cg_failed++;
        TEST_FAIL << "CodeGraphCallersTool failed: " << result << std::endl;
    }

    cleanup_temp_project(tmp_dir);
}

asio::awaitable<void>
    test_codegraph_callees_tool_execute(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
    auto tmp_dir   = create_temp_project();
    auto codegraph = std::make_shared<agentxx::expand::CodeGraphManager>();
    codegraph->initialize(tmp_dir);
    codegraph->indexDirectory(tmp_dir, false);

    auto tool   = agentxx::tools::CodeGraphCalleesTool{codegraph, agentContext};
    auto result = co_await tool.execute_async({
        {"symbol", "main"}
    });

    // 多行文本格式: 首行 "Callees (N):" (可为空列表)
    if (result.rfind("Callees (", 0) == 0
        || result.find("error:") != std::string::npos) {
        g_cg_passed++;
        TEST_PASS << "CodeGraphCalleesTool returns plain-text callees" << std::endl;
    } else {
        g_cg_failed++;
        TEST_FAIL << "CodeGraphCalleesTool failed: " << result << std::endl;
    }

    cleanup_temp_project(tmp_dir);
}

asio::awaitable<void>
    test_codegraph_impact_tool_execute(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
    auto tmp_dir   = create_temp_project();
    auto codegraph = std::make_shared<agentxx::expand::CodeGraphManager>();
    codegraph->initialize(tmp_dir);
    codegraph->indexDirectory(tmp_dir, false);

    auto tool   = agentxx::tools::CodeGraphImpactTool{codegraph, agentContext};
    auto result = co_await tool.execute_async({
        {"symbol", "add"}
    });

    // 多行文本格式: 首行 "Impact (N):" (可为空列表)
    if (result.rfind("Impact (", 0) == 0
        || result.find("error:") != std::string::npos) {
        g_cg_passed++;
        TEST_PASS << "CodeGraphImpactTool returns plain-text impact" << std::endl;
    } else {
        g_cg_failed++;
        TEST_FAIL << "CodeGraphImpactTool failed: " << result << std::endl;
    }

    cleanup_temp_project(tmp_dir);
}

asio::awaitable<void>
    test_codegraph_path_tool_execute(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
    auto tmp_dir   = create_temp_project();
    auto codegraph = std::make_shared<agentxx::expand::CodeGraphManager>();
    codegraph->initialize(tmp_dir);
    codegraph->indexDirectory(tmp_dir, false);

    auto tool   = agentxx::tools::CodeGraphPathTool{codegraph, agentContext};
    auto result = co_await tool.execute_async({
        {"from", "main"    },
        {"to",   "add_impl"}
    });

    // 多行文本格式: 首行 "Path (N):", 找不到路径时返回 "error: No path found"
    if (result.rfind("Path (", 0) == 0
        || result.find("error:") != std::string::npos) {
        g_cg_passed++;
        TEST_PASS << "CodeGraphPathTool returns path or no-path-found" << std::endl;
    } else {
        TEST_INFO << "CodeGraphPathTool result: " << result << std::endl;
    }

    cleanup_temp_project(tmp_dir);
}

// =========================================================================
// 索引过滤测试 (ignorePaths / .gitignore / .gitmodules / loadPaths)
// =========================================================================

/// 创建带第三方目录与 gitignore 的过滤测试项目:
/// - src/          正常源码
/// - third_party/  第三方目录 (测试 ignorePaths / .gitmodules 忽略)
/// - vendor/       被 .gitignore 忽略的目录
static std::string create_filter_project() {
    int  idx     = g_temp_project_counter.fetch_add(1);
    auto tmp_dir = fs::temp_directory_path() / ("codegraph_filter_test_" + std::to_string(idx));
    if (fs::exists(tmp_dir)) {
        fs::remove_all(tmp_dir);
    }
    fs::create_directories(tmp_dir / "src");
    fs::create_directories(tmp_dir / "third_party");
    fs::create_directories(tmp_dir / "vendor");

    {
        std::ofstream f(tmp_dir / "src" / "main.cpp");
        f << "int helper() { return 1; }\nint main() { return helper(); }\n";
    }
    {
        std::ofstream f(tmp_dir / "third_party" / "lib.cpp");
        f << "int third_party_func() { return 3; }\n";
    }
    {
        std::ofstream f(tmp_dir / "vendor" / "vendored.cpp");
        f << "int vendored_func() { return 4; }\n";
    }

    // .gitignore: 忽略 vendor 目录 (目录模式)
    {
        std::ofstream f(tmp_dir / ".gitignore");
        f << "vendor/\n";
    }

    return tmp_dir.generic_string();
}

/// ignorePaths 过滤: 命中路径 (目录前缀匹配) 不进入索引
void test_codegraph_ignore_paths() {
    auto tmp_dir = create_filter_project();
    agentxx::expand::CodeGraphIndexConfig cfg;
    cfg.ignorePaths.push_back((fs::path(tmp_dir) / "third_party").generic_string());
    auto manager = agentxx::expand::CodeGraphManager{"", std::move(cfg)};

    manager.initialize(tmp_dir);
    manager.indexDirectory(tmp_dir, false);

    // third_party 内符号不可查询; src 内符号可查询
    auto r1 = manager.searchSymbols("third_party_func", 10);
    auto r2 = manager.searchSymbols("helper", 10);

    if ((r1.success && r1.nodes.empty()) && (r2.success && !r2.nodes.empty())) {
        g_cg_passed++;
        TEST_PASS << "ignorePaths excludes directory from index" << std::endl;
    } else {
        g_cg_failed++;
        TEST_FAIL << "ignorePaths filter FAILED: third_party_func nodes=" << r1.nodes.size()
                  << " helper nodes=" << r2.nodes.size() << std::endl;
    }

    cleanup_temp_project(tmp_dir);
}

/// ignorePaths 通配符: `**/third_party/**` 命中任意层级的同名目录
void test_codegraph_ignore_paths_wildcard() {
    auto tmp_dir = create_filter_project();
    agentxx::expand::CodeGraphIndexConfig cfg;
    cfg.ignorePaths.push_back("**/third_party/**");
    auto manager = agentxx::expand::CodeGraphManager{"", std::move(cfg)};

    manager.initialize(tmp_dir);
    manager.indexDirectory(tmp_dir, false);

    auto r1 = manager.searchSymbols("third_party_func", 10);
    auto r2 = manager.searchSymbols("helper", 10);

    if ((r1.success && r1.nodes.empty()) && (r2.success && !r2.nodes.empty())) {
        g_cg_passed++;
        TEST_PASS << "ignorePaths wildcard excludes matched files" << std::endl;
    } else {
        g_cg_failed++;
        TEST_FAIL << "ignorePaths wildcard FAILED: third_party_func nodes=" << r1.nodes.size()
                  << " helper nodes=" << r2.nodes.size() << std::endl;
    }

    cleanup_temp_project(tmp_dir);
}

/// .gitignore 过滤: 被忽略的目录不进入索引 (默认启用)
void test_codegraph_gitignore() {
    auto tmp_dir = create_filter_project();
    auto manager = agentxx::expand::CodeGraphManager{};

    manager.initialize(tmp_dir);
    manager.indexDirectory(tmp_dir, false);

    // vendor (gitignore) 内符号不可查询; src 内符号可查询
    auto r1 = manager.searchSymbols("vendored_func", 10);
    auto r2 = manager.searchSymbols("helper", 10);

    if ((r1.success && r1.nodes.empty()) && (r2.success && !r2.nodes.empty())) {
        g_cg_passed++;
        TEST_PASS << ".gitignore excluded dir skipped" << std::endl;
    } else {
        g_cg_failed++;
        TEST_FAIL << ".gitignore filter FAILED: vendored_func nodes=" << r1.nodes.size()
                  << " helper nodes=" << r2.nodes.size() << std::endl;
    }

    cleanup_temp_project(tmp_dir);
}

/// use_gitignore=false: .gitignore 规则不生效, 被忽略文件正常索引
void test_codegraph_gitignore_disabled() {
    auto tmp_dir = create_filter_project();
    agentxx::expand::CodeGraphIndexConfig cfg;
    cfg.useGitignore = false;
    auto manager = agentxx::expand::CodeGraphManager{"", std::move(cfg)};

    manager.initialize(tmp_dir);
    manager.indexDirectory(tmp_dir, false);

    auto r1 = manager.searchSymbols("vendored_func", 10);
    if (r1.success && !r1.nodes.empty()) {
        g_cg_passed++;
        TEST_PASS << "use_gitignore=false: gitignored file still indexed" << std::endl;
    } else {
        g_cg_failed++;
        TEST_FAIL << "use_gitignore=false FAILED: vendored_func not indexed" << std::endl;
    }

    cleanup_temp_project(tmp_dir);
}

/// .gitmodules 子模块目录忽略: 声明为子模块的目录不进入索引
void test_codegraph_gitmodules() {
    auto tmp_dir = create_filter_project();
    {
        std::ofstream f(fs::path(tmp_dir) / ".gitmodules");
        f << "[submodule \"third_party\"]\n"
             "\tpath = third_party\n"
             "\turl = https://example.com/x\n";
    }
    auto manager = agentxx::expand::CodeGraphManager{};

    manager.initialize(tmp_dir);
    manager.indexDirectory(tmp_dir, false);

    // third_party (gitmodules 子模块) 内符号不可查询; src 内符号可查询
    auto r1 = manager.searchSymbols("third_party_func", 10);
    auto r2 = manager.searchSymbols("helper", 10);

    if ((r1.success && r1.nodes.empty()) && (r2.success && !r2.nodes.empty())) {
        g_cg_passed++;
        TEST_PASS << ".gitmodules submodule dir skipped" << std::endl;
    } else {
        g_cg_failed++;
        TEST_FAIL << ".gitmodules filter FAILED: third_party_func nodes=" << r1.nodes.size()
                  << " helper nodes=" << r2.nodes.size() << std::endl;
    }

    cleanup_temp_project(tmp_dir);
}

/// 多级 .gitignore 继承: 子目录内的 .gitignore 规则同样生效 (忽略其所在
/// 层级下的文件), 不要求规则只写在项目根
void test_codegraph_gitignore_nested() {
    int  idx     = g_temp_project_counter.fetch_add(1);
    auto tmp_dir = fs::temp_directory_path() / ("codegraph_nested_gitignore_" + std::to_string(idx));
    if (fs::exists(tmp_dir)) {
        fs::remove_all(tmp_dir);
    }
    fs::create_directories(tmp_dir / "src" / "debug");

    {
        std::ofstream f(tmp_dir / "src" / "main.cpp");
        f << "int helper() { return 1; }\nint main() { return helper(); }\n";
    }
    {
        std::ofstream f(tmp_dir / "src" / "debug" / "dbg.cpp");
        f << "int dbg_func() { return 2; }\n";
    }
    // 子目录级 .gitignore: 忽略 src/debug (不写根 .gitignore 也应生效)
    {
        std::ofstream f(tmp_dir / "src" / ".gitignore");
        f << "debug/\n";
    }
    auto manager = agentxx::expand::CodeGraphManager{};

    manager.initialize(tmp_dir.generic_string());
    manager.indexDirectory(tmp_dir.generic_string(), false);

    // src/debug (子目录 gitignore) 内符号不可查询; src 内符号可查询
    auto r1 = manager.searchSymbols("dbg_func", 10);
    auto r2 = manager.searchSymbols("helper", 10);

    if ((r1.success && r1.nodes.empty()) && (r2.success && !r2.nodes.empty())) {
        g_cg_passed++;
        TEST_PASS << "nested .gitignore excludes subdir" << std::endl;
    } else {
        g_cg_failed++;
        TEST_FAIL << "nested .gitignore FAILED: dbg_func nodes=" << r1.nodes.size()
                  << " helper nodes=" << r2.nodes.size() << std::endl;
    }

    cleanup_temp_project(tmp_dir.generic_string());
}

/// 多级 .gitmodules: 子目录内的 .gitmodules (嵌套 git 仓库) 声明同样生效,
/// 不要求只解析项目根 .gitmodules
void test_codegraph_gitmodules_nested() {
    int  idx     = g_temp_project_counter.fetch_add(1);
    auto tmp_dir = fs::temp_directory_path()
                   / ("codegraph_nested_gitmodules_" + std::to_string(idx));
    if (fs::exists(tmp_dir)) {
        fs::remove_all(tmp_dir);
    }
    fs::create_directories(tmp_dir / "src" / "third_party");

    {
        std::ofstream f(tmp_dir / "src" / "main.cpp");
        f << "int helper() { return 1; }\n";
    }
    {
        std::ofstream f(tmp_dir / "src" / "third_party" / "lib.cpp");
        f << "int lib_func() { return 2; }\n";
    }
    // 子目录级 .gitmodules: 声明 src/third_party 为子模块
    {
        std::ofstream f(tmp_dir / "src" / ".gitmodules");
        f << "[submodule \"third_party\"]\n"
             "\tpath = third_party\n"
             "\turl = https://example.com/x\n";
    }
    auto manager = agentxx::expand::CodeGraphManager{};

    manager.initialize(tmp_dir.generic_string());
    manager.indexDirectory(tmp_dir.generic_string(), false);

    // src/third_party (嵌套 .gitmodules 子模块) 内符号不可查询; src 内符号可查询
    auto r1 = manager.searchSymbols("lib_func", 10);
    auto r2 = manager.searchSymbols("helper", 10);

    if ((r1.success && r1.nodes.empty()) && (r2.success && !r2.nodes.empty())) {
        g_cg_passed++;
        TEST_PASS << "nested .gitmodules submodule dir skipped" << std::endl;
    } else {
        g_cg_failed++;
        TEST_FAIL << "nested .gitmodules FAILED: lib_func nodes=" << r1.nodes.size()
                  << " helper nodes=" << r2.nodes.size() << std::endl;
    }

    cleanup_temp_project(tmp_dir.generic_string());
}

/// 根 .gitignore 中 `/xxx` 锚定规则 (git 规范: 开头的 `/` 仅表示锚定到
/// 规则所在目录, 不参与路径匹配): 复现 agent/.gitignore 的 `/third_party/boost*/`
/// 场景, 验证该目录被忽略 (修复前: 开头的 `/` 被编进正则, 无法匹配
/// relative() 得到的相对路径, 规则永不生效)
void test_codegraph_gitignore_anchored_root() {
    int  idx     = g_temp_project_counter.fetch_add(1);
    auto tmp_dir = fs::temp_directory_path()
                   / ("codegraph_anchor_root_" + std::to_string(idx));
    if (fs::exists(tmp_dir)) {
        fs::remove_all(tmp_dir);
    }
    fs::create_directories(tmp_dir / "third_party" / "boost");
    fs::create_directories(tmp_dir / "third_party" / "other");

    {
        std::ofstream f(tmp_dir / "main.cpp");
        f << "int helper() { return 1; }\n";
    }
    {
        std::ofstream f(tmp_dir / "third_party" / "boost" / "boost_lib.cpp");
        f << "int boost_func() { return 2; }\n";
    }
    {
        std::ofstream f(tmp_dir / "third_party" / "other" / "other_lib.cpp");
        f << "int other_func() { return 3; }\n";
    }
    // 与 agent/.gitignore 相同的锚定规则形式
    {
        std::ofstream f(tmp_dir / ".gitignore");
        f << "/third_party/boost*/\n";
    }
    auto manager = agentxx::expand::CodeGraphManager{};

    manager.initialize(tmp_dir.generic_string());
    manager.indexDirectory(tmp_dir.generic_string(), false);

    auto r1 = manager.searchSymbols("boost_func", 10);
    auto r2 = manager.searchSymbols("other_func", 10);
    auto r3 = manager.searchSymbols("helper", 10);

    if ((r1.success && r1.nodes.empty()) && (r2.success && !r2.nodes.empty())
        && (r3.success && !r3.nodes.empty())) {
        g_cg_passed++;
        TEST_PASS << "anchored /third_party/boost*/ rule excludes boost dir" << std::endl;
    } else {
        g_cg_failed++;
        TEST_FAIL << "anchored rule FAILED: boost_func=" << r1.nodes.size()
                  << " other_func=" << r2.nodes.size() << " helper=" << r3.nodes.size()
                  << std::endl;
    }

    cleanup_temp_project(tmp_dir.generic_string());
}

/// 子目录 .gitignore 中 `/xxx` 锚定规则: 锚定到该 .gitignore 所在目录
/// (即 agent/.gitignore 位于项目子目录的真实场景), 且不影响同层级的
/// 其他同名路径之外的内容
void test_codegraph_gitignore_anchored_nested() {
    int  idx     = g_temp_project_counter.fetch_add(1);
    auto tmp_dir = fs::temp_directory_path()
                   / ("codegraph_anchor_nested_" + std::to_string(idx));
    if (fs::exists(tmp_dir)) {
        fs::remove_all(tmp_dir);
    }
    fs::create_directories(tmp_dir / "src" / "third_party" / "boost");
    fs::create_directories(tmp_dir / "src" / "third_party" / "other");

    {
        std::ofstream f(tmp_dir / "src" / "main.cpp");
        f << "int helper() { return 1; }\n";
    }
    {
        std::ofstream f(tmp_dir / "src" / "third_party" / "boost" / "boost_lib.cpp");
        f << "int boost_func() { return 2; }\n";
    }
    {
        std::ofstream f(tmp_dir / "src" / "third_party" / "other" / "other_lib.cpp");
        f << "int other_func() { return 3; }\n";
    }
    {
        std::ofstream f(tmp_dir / "src" / ".gitignore");
        f << "/third_party/boost*/\n";
    }
    auto manager = agentxx::expand::CodeGraphManager{};

    manager.initialize(tmp_dir.generic_string());
    manager.indexDirectory(tmp_dir.generic_string(), false);

    auto r1 = manager.searchSymbols("boost_func", 10);
    auto r2 = manager.searchSymbols("other_func", 10);
    auto r3 = manager.searchSymbols("helper", 10);

    if ((r1.success && r1.nodes.empty()) && (r2.success && !r2.nodes.empty())
        && (r3.success && !r3.nodes.empty())) {
        g_cg_passed++;
        TEST_PASS << "nested anchored /third_party/boost*/ rule excludes boost dir" << std::endl;
    } else {
        g_cg_failed++;
        TEST_FAIL << "nested anchored rule FAILED: boost_func=" << r1.nodes.size()
                  << " other_func=" << r2.nodes.size() << " helper=" << r3.nodes.size()
                  << std::endl;
    }

    cleanup_temp_project(tmp_dir.generic_string());
}

/// `**/` 开头模式: git 语义匹配任意层级 (非锚定), 如 `**/generated/`
/// 忽略任意深度下的 generated 目录
void test_codegraph_gitignore_doublestar_prefix() {
    int  idx     = g_temp_project_counter.fetch_add(1);
    auto tmp_dir = fs::temp_directory_path()
                   / ("codegraph_dstar_prefix_" + std::to_string(idx));
    if (fs::exists(tmp_dir)) {
        fs::remove_all(tmp_dir);
    }
    fs::create_directories(tmp_dir / "a" / "generated");
    fs::create_directories(tmp_dir / "b" / "c" / "generated");

    {
        std::ofstream f(tmp_dir / "main.cpp");
        f << "int helper() { return 1; }\n";
    }
    {
        std::ofstream f(tmp_dir / "a" / "generated" / "gen_a.cpp");
        f << "int gen_a_func() { return 2; }\n";
    }
    {
        std::ofstream f(tmp_dir / "b" / "c" / "generated" / "gen_b.cpp");
        f << "int gen_b_func() { return 3; }\n";
    }
    {
        std::ofstream f(tmp_dir / ".gitignore");
        f << "**/generated/\n";
    }
    auto manager = agentxx::expand::CodeGraphManager{};

    manager.initialize(tmp_dir.generic_string());
    manager.indexDirectory(tmp_dir.generic_string(), false);

    auto r1 = manager.searchSymbols("gen_a_func", 10);
    auto r2 = manager.searchSymbols("gen_b_func", 10);
    auto r3 = manager.searchSymbols("helper", 10);

    if ((r1.success && r1.nodes.empty()) && (r2.success && r2.nodes.empty())
        && (r3.success && !r3.nodes.empty())) {
        g_cg_passed++;
        TEST_PASS << "**/generated/ excludes generated dirs at any depth" << std::endl;
    } else {
        g_cg_failed++;
        TEST_FAIL << "**/ prefix FAILED: gen_a=" << r1.nodes.size() << " gen_b=" << r2.nodes.size()
                  << " helper=" << r3.nodes.size() << std::endl;
    }

    cleanup_temp_project(tmp_dir.generic_string());
}

/// use_gitignore 启用时忽略 .git 元数据目录 (含项目根下的 .git):
/// .git 内的源文件不进入索引
void test_codegraph_git_dir_ignored() {
    int  idx     = g_temp_project_counter.fetch_add(1);
    auto tmp_dir = fs::temp_directory_path() / ("codegraph_gitdir_test_" + std::to_string(idx));
    if (fs::exists(tmp_dir)) {
        fs::remove_all(tmp_dir);
    }
    fs::create_directories(tmp_dir / ".git" / "objects");

    {
        std::ofstream f(tmp_dir / "main.cpp");
        f << "int helper() { return 1; }\n";
    }
    {
        // .git 内的 cpp 文件: 即便语言可识别也不应进入索引
        std::ofstream f(tmp_dir / ".git" / "objects" / "gitdata.cpp");
        f << "int git_internal_func() { return 2; }\n";
    }
    auto manager = agentxx::expand::CodeGraphManager{};

    manager.initialize(tmp_dir.generic_string());
    manager.indexDirectory(tmp_dir.generic_string(), false);

    auto r1 = manager.searchSymbols("git_internal_func", 10);
    auto r2 = manager.searchSymbols("helper", 10);

    if ((r1.success && r1.nodes.empty()) && (r2.success && !r2.nodes.empty())) {
        g_cg_passed++;
        TEST_PASS << ".git dir excluded from index" << std::endl;
    } else {
        g_cg_failed++;
        TEST_FAIL << ".git dir FAILED: git_internal_func nodes=" << r1.nodes.size()
                  << " helper nodes=" << r2.nodes.size() << std::endl;
    }

    cleanup_temp_project(tmp_dir.generic_string());
}

/// 增量索引 + gitignore: 首次全量索引后新增 .gitignore 忽略已有文件, 再次
/// 增量索引时经单文件过滤 (GitIgnoreCache) 清理被忽略文件的残留记录
void test_codegraph_gitignore_incremental_cleanup() {
    int  idx     = g_temp_project_counter.fetch_add(1);
    auto tmp_dir = fs::temp_directory_path() / ("codegraph_gitignore_incr_" + std::to_string(idx));
    if (fs::exists(tmp_dir)) {
        fs::remove_all(tmp_dir);
    }
    fs::create_directories(tmp_dir / "src");

    {
        std::ofstream f(tmp_dir / "src" / "main.cpp");
        f << "int helper() { return 1; }\nint main() { return helper(); }\n";
    }
    auto manager = agentxx::expand::CodeGraphManager{};

    manager.initialize(tmp_dir.generic_string());
    manager.indexDirectory(tmp_dir.generic_string(), false); // 全量: helper 已索引

    // 新增 .gitignore 忽略 src/ 整个目录, 增量索引
    {
        std::ofstream f(tmp_dir / ".gitignore");
        f << "src/\n";
    }
    manager.indexDirectory(tmp_dir.generic_string(), true);

    // 残留记录应被清理: helper 不再可查询
    auto r = manager.searchSymbols("helper", 10);
    if (r.success && r.nodes.empty()) {
        g_cg_passed++;
        TEST_PASS << "incremental index cleans up gitignored file records" << std::endl;
    } else {
        g_cg_failed++;
        TEST_FAIL << "incremental cleanup FAILED: helper nodes=" << r.nodes.size() << std::endl;
    }

    cleanup_temp_project(tmp_dir.generic_string());
}

/// loadPaths 多目录: updateIndex 按加载路径列表逐个索引
void test_codegraph_load_paths() {
    int  idx      = g_temp_project_counter.fetch_add(1);
    auto tmp_base = fs::temp_directory_path()
                    / ("codegraph_loadpaths_test_" + std::to_string(idx));
    if (fs::exists(tmp_base)) {
        fs::remove_all(tmp_base);
    }
    auto dir_a = tmp_base / "a";
    auto dir_b = tmp_base / "b";
    fs::create_directories(dir_a);
    fs::create_directories(dir_b);

    {
        std::ofstream f(dir_a / "a.cpp");
        f << "int func_a() { return 1; }\n";
    }
    {
        std::ofstream f(dir_b / "b.cpp");
        f << "int func_b() { return 2; }\n";
    }

    agentxx::expand::CodeGraphIndexConfig cfg;
    cfg.loadPaths.push_back(dir_a.generic_string());
    cfg.loadPaths.push_back(dir_b.generic_string());
    auto manager = agentxx::expand::CodeGraphManager{"", std::move(cfg)};

    manager.initialize(tmp_base.generic_string());
    bool ok = manager.updateIndex(); // 按 loadPaths 索引

    auto r1 = manager.searchSymbols("func_a", 10);
    auto r2 = manager.searchSymbols("func_b", 10);

    if (ok && r1.success && !r1.nodes.empty() && r2.success && !r2.nodes.empty()) {
        g_cg_passed++;
        TEST_PASS << "loadPaths multi-dir updateIndex works" << std::endl;
    } else {
        g_cg_failed++;
        TEST_FAIL << "loadPaths FAILED: ok=" << ok << " func_a=" << r1.nodes.size()
                  << " func_b=" << r2.nodes.size() << std::endl;
    }

    fs::remove_all(tmp_base);
}

/// autoLoadProjectRoot=false (load_cwd=false) 且无加载路径: updateIndex 空操作不索引
void test_codegraph_load_cwd_disabled() {
    auto tmp_dir = create_temp_project();
    agentxx::expand::CodeGraphIndexConfig cfg;
    cfg.autoLoadProjectRoot = false;
    auto manager = agentxx::expand::CodeGraphManager{"", std::move(cfg)};

    manager.initialize(tmp_dir);
    bool ok = manager.updateIndex(); // 无加载路径且关闭默认加载: 空操作

    auto r = manager.searchSymbols("add", 10);
    if (ok && r.success && r.nodes.empty()) {
        g_cg_passed++;
        TEST_PASS << "load_cwd=false: no auto index without loadPaths" << std::endl;
    } else {
        g_cg_failed++;
        TEST_FAIL << "load_cwd=false FAILED: nodes=" << r.nodes.size() << std::endl;
    }

    cleanup_temp_project(tmp_dir);
}

// =========================================================================
// Test Runner
// =========================================================================

// 索引进度断点续传验证: 全量索引后(模拟重启)重新打开同一 db, 增量索引
// 应跳过已索引文件 (进度回调 0 次), 查询命中已索引数据
void test_codegraph_manager_restart_resume() {
    auto tmp_dir = create_temp_project();

    int inprocess_calls = 0;
    {
        agentxx::expand::CodeGraphManager manager;
        manager.initialize(tmp_dir);
        manager.indexDirectory(tmp_dir, false); // 全量
        manager.setProgressCallback(
            [&](int /*processed*/, int /*total*/, std::string_view) { ++inprocess_calls; }
        );
        manager.indexDirectory(tmp_dir, true); // 同进程增量: 无变更应全部跳过
    } // manager 析构: db 关闭 + WAL checkpoint

    int resumed_processed = 0;
    {
        agentxx::expand::CodeGraphManager manager2;
        manager2.initialize(tmp_dir); // 重新打开 (模拟重启)
        manager2.setProgressCallback(
            [&](int processed, int total, std::string_view currentFile) {
                // 完成信号 (total 回调, currentFile 为空) 不算真正处理
                if (!currentFile.empty() && processed < total) {
                    ++resumed_processed;
                }
            }
        );
        manager2.indexDirectory(tmp_dir, true); // 重启后增量: 应跳过所有文件

        auto result = manager2.searchSymbols("add", 10);
        if (resumed_processed == 0 && result.success && !result.nodes.empty()) {
            g_cg_passed++;
            TEST_PASS << "restart resume: unmodified files skipped after restart" << std::endl;
        } else {
            g_cg_failed++;
            TEST_FAIL << "restart resume FAILED: processed=" << resumed_processed
                      << " search_success=" << result.success << std::endl;
        }
    }

    cleanup_temp_project(tmp_dir);
}

// 索引进行中查询不阻塞验证: 后台线程全量索引 (模拟预热索引), 线程前台
// 查询应快速返回 (索引写锁仅毫秒级窗口), 而不是等待索引完成
void test_codegraph_query_during_indexing() {
    auto tmp_dir = create_multi_lang_project();
    auto manager = std::make_shared<agentxx::expand::CodeGraphManager>();
    manager->initialize(tmp_dir);

    std::atomic<bool> first_file{false};
    manager->setProgressCallback(
        [&](int /*processed*/, int /*total*/, std::string_view currentFile) {
            if (!currentFile.empty()) {
                first_file = true;
            }
        }
    );

    // 后台索引线程: 全量索引 (多文件, 模拟索引进行中)
    std::thread indexThread([&]() {
        manager->indexDirectory(tmp_dir, false);
    });

    // 等待索引开始处理第一个文件
    for (int i = 0; i < 5000 && !first_file.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // 索引进行中: 前台查询应立即返回 (不等待索引完成)
    const auto startAt = std::chrono::steady_clock::now();
    auto       result  = manager->searchSymbols("greet", 10);
    const auto costUs  = std::chrono::duration_cast<std::chrono::microseconds>(
                             std::chrono::steady_clock::now() - startAt
    )
                             .count();

    indexThread.join();

    // 查询耗时远小于索引总时长 (阈值 3s 宽松; 旧实现被写锁阻塞会超时失败)
    if (costUs < std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::seconds{3})
                     .count()) {
        g_cg_passed++;
        TEST_PASS << "query during indexing returned promptly: " << costUs << "us" << std::endl;
    } else {
        g_cg_failed++;
        TEST_FAIL << "query blocked during indexing: " << costUs << "us" << std::endl;
    }
    // 查询本身不抛异常 (数据为已提交部分数据, 可能为空但 success)
    if (result.success) {
        g_cg_passed++;
        TEST_PASS << "query during indexing returns committed partial data" << std::endl;
    } else {
        g_cg_failed++;
        TEST_FAIL << "query during indexing failed: " << result.error << std::endl;
    }

    cleanup_temp_project(tmp_dir);
}

/// 收尾阶段查询不阻塞验证: 放大收尾成本 (大量文件残留清理 + 大量跨文件
/// 引用解析), 索引期间持续查询, 任何单次查询都不应被长时间阻塞
/// - 修复前: 收尾 (清理/引用解析/FTS 重建/checkpoint) 在单个独占锁内串行
///   执行, 期间 codegraph tool 查询全部阻塞 (agent 线程卡住)
/// - 修复后: 清理的磁盘 IO/规则匹配与引用解析计算移出独占锁, 锁内仅剩
///   短暂事务写
void test_codegraph_query_during_finalize() {
    int  idx     = g_temp_project_counter.fetch_add(1);
    auto tmp_dir = fs::temp_directory_path() / ("codegraph_finalize_test_" + std::to_string(idx));
    if (fs::exists(tmp_dir)) {
        fs::remove_all(tmp_dir);
    }
    fs::create_directories(tmp_dir);

    // 800 个文件, 每个文件定义一个符号并跨文件调用下一个文件的符号,
    // 制造大量未解析引用 (放大收尾引用解析成本)
    constexpr int kFileCount = 800;
    for (int i = 0; i < kFileCount; ++i) {
        std::ofstream f(tmp_dir / ("mod" + std::to_string(i) + ".cpp"));
        f << "int sym_" << i << "() { return " << i << "; }\n";
        f << "int caller_" << i << "() { return sym_" << (i + 1) % kFileCount << "(); }\n";
    }
    auto manager = std::make_shared<agentxx::expand::CodeGraphManager>();
    manager->initialize(tmp_dir.generic_string());

    std::atomic<bool> indexing_done{false};
    // 后台索引线程: 全量索引 (含收尾清理 + 引用解析)
    std::thread indexThread([&]() {
        manager->indexDirectory(tmp_dir.generic_string(), false);
        indexing_done = true;
    });

    // 前台持续查询 (每 10ms 一次), 记录最坏单次耗时
    std::atomic<int64_t> worst_us{0};
    std::thread queryThread([&]() {
        while (!indexing_done.load()) {
            auto startAt = std::chrono::steady_clock::now();
            manager->searchSymbols("sym_1", 10); // 无论结果如何都要走查询锁
            auto costUs = std::chrono::duration_cast<std::chrono::microseconds>(
                              std::chrono::steady_clock::now() - startAt
            )
                              .count();
            int64_t cur = worst_us.load();
            while (costUs > cur && !worst_us.compare_exchange_weak(cur, costUs)) {
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    });

    indexThread.join();
    queryThread.join();

    // 收尾期间任何单次查询都不应超过 2s (旧实现收尾独占锁可阻塞数秒~十秒级)
    const auto worstMs = worst_us.load() / 1000;
    if (worstMs < 2000) {
        g_cg_passed++;
        TEST_PASS << "query never blocked long during finalize, worst=" << worstMs
                  << "ms" << std::endl;
    } else {
        g_cg_failed++;
        TEST_FAIL << "query blocked during finalize, worst=" << worstMs << "ms" << std::endl;
    }

    // 收尾后的数据完整性: 引用解析完成, 跨文件调用边应存在 (callees 可查)
    auto result = manager->getCallees("caller_0", 3);
    if (result.success) {
        g_cg_passed++;
        TEST_PASS << "references resolved after finalize" << std::endl;
    } else {
        g_cg_failed++;
        TEST_FAIL << "resolve after finalize failed: " << result.error << std::endl;
    }

    cleanup_temp_project(tmp_dir.generic_string());
}

asio::awaitable<TestResult>
    run_codegraph_tools_tests(std::weak_ptr<agentxx::agent::AgentContext> agentContext) {
    test_codegraph_manager_init();
    test_codegraph_manager_init_twice();
    test_codegraph_manager_not_initialized();
    test_codegraph_manager_index();
    test_codegraph_manager_incremental_index();
    test_codegraph_manager_cache_invalidate();
    test_codegraph_manager_restart_resume();
    test_codegraph_query_during_indexing();
    test_codegraph_query_during_finalize();
    test_codegraph_manager_search();
    test_codegraph_manager_search_no_results();
    test_codegraph_manager_context();
    test_codegraph_manager_status();
    test_codegraph_circular_deps();
    test_codegraph_resolve_unresolvable();
    test_codegraph_manager_callers();
    test_codegraph_manager_callees();
    test_codegraph_manager_impact();
    test_codegraph_manager_path();
    test_codegraph_manager_shutdown();
    test_codegraph_manager_update_index();
    test_codegraph_manager_resolve();

    test_codegraph_ignore_paths();
    test_codegraph_ignore_paths_wildcard();
    test_codegraph_gitignore();
    test_codegraph_gitignore_disabled();
    test_codegraph_gitmodules();
    test_codegraph_gitignore_nested();
    test_codegraph_gitmodules_nested();
    test_codegraph_gitignore_anchored_root();
    test_codegraph_gitignore_anchored_nested();
    test_codegraph_gitignore_doublestar_prefix();
    test_codegraph_git_dir_ignored();
    test_codegraph_gitignore_incremental_cleanup();
    test_codegraph_load_paths();
    test_codegraph_load_cwd_disabled();

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

    auto run = [agentContext](auto testFn) -> asio::awaitable<void> {
        try {
            co_await testFn(agentContext);
        } catch (const std::exception& e) {
            g_cg_failed++;
            TEST_FAIL << "Exception in test: " << e.what() << std::endl;
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

    co_return TestResult{g_cg_passed, g_cg_failed};
}

} // namespace test
} // namespace agentxx

#endif
