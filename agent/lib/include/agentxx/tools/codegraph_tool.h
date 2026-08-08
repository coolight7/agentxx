#pragma once

#include "agentxx/tools/tool.h"
#include <memory>
#include <string>

#if AGENTXX_ENABLE_CODEGRAPH

namespace agentxx {
namespace expand {
class CodeGraphManager;
} // namespace expand

namespace tools {

// 代码符号搜索 tool（agentxx_codegraph_search）
// 功能：按关键字模糊搜索代码符号（函数、类、变量、宏等），返回符号类型、
//       名称、限定名、所在文件与行号、签名等信息。
// 参数：
//   - query（必填）：搜索关键字
//   - limit（可选）：返回结果数量上限，默认 20
// 用途：快速定位代码符号的定义位置，是其他 codegraph 查询的入口
class CodeGraphSearchTool : public XXToolBase {
protected:

    std::shared_ptr<agentxx::expand::CodeGraphManager> codegraph;

public:

    CodeGraphSearchTool(
        std::shared_ptr<agentxx::expand::CodeGraphManager> in_codegraph,
        std::weak_ptr<agentxx::agent::AgentContext>        in_agentContext
    );

    neograph::ChatTool get_definition() const override;

    asio::awaitable<std::string> execute_async(const neograph::json& arguments) override;
};

// 符号上下文 tool（agentxx_codegraph_context）
// 功能：获取指定符号的上下文信息，包括其定义、引用点以及按调用关系深度遍历
//       到的上下游关联符号，帮助理解该符号在整个代码库中的关联全貌。
// 参数：
//   - symbol（必填）：目标符号名称
//   - limit（可选）：每个关联层返回的符号数量上限，默认 10
//   - max_depth（可选）：关联遍历最大深度，默认 3
// 用途：阅读代码时快速了解一个符号的完整关联关系
class CodeGraphContextTool : public XXToolBase {
protected:

    std::shared_ptr<agentxx::expand::CodeGraphManager> codegraph;

public:

    CodeGraphContextTool(
        std::shared_ptr<agentxx::expand::CodeGraphManager> in_codegraph,
        std::weak_ptr<agentxx::agent::AgentContext>        in_agentContext
    );

    neograph::ChatTool get_definition() const override;

    asio::awaitable<std::string> execute_async(const neograph::json& arguments) override;
};

// 调用者查询 tool（agentxx_codegraph_callers）
// 功能：沿反向调用链查找所有调用指定符号的上游调用者，回答"谁调用了它"。
// 参数：
//   - symbol（必填）：目标符号名称
//   - max_depth（可选）：向上追溯的最大深度，默认 3
// 用途：分析符号被哪些位置引用/调用，评估改动时影响的上游代码
class CodeGraphCallersTool : public XXToolBase {
protected:

    std::shared_ptr<agentxx::expand::CodeGraphManager> codegraph;

public:

    CodeGraphCallersTool(
        std::shared_ptr<agentxx::expand::CodeGraphManager> in_codegraph,
        std::weak_ptr<agentxx::agent::AgentContext>        in_agentContext
    );

    neograph::ChatTool get_definition() const override;

    asio::awaitable<std::string> execute_async(const neograph::json& arguments) override;
};

// 被调用者查询 tool（agentxx_codegraph_callees）
// 功能：沿正向调用链查找指定符号调用的所有下游被调用者，回答"它调用了谁"。
// 参数：
//   - symbol（必填）：目标符号名称
//   - max_depth（可选）：向下追溯的最大深度，默认 3
// 用途：分析符号依赖了哪些其他符号，梳理代码的依赖结构
class CodeGraphCalleesTool : public XXToolBase {
protected:

    std::shared_ptr<agentxx::expand::CodeGraphManager> codegraph;

public:

    CodeGraphCalleesTool(
        std::shared_ptr<agentxx::expand::CodeGraphManager> in_codegraph,
        std::weak_ptr<agentxx::agent::AgentContext>        in_agentContext
    );

    neograph::ChatTool get_definition() const override;

    asio::awaitable<std::string> execute_async(const neograph::json& arguments) override;
};

// 影响面分析 tool（agentxx_codegraph_impact）
// 功能：综合分析指定符号的调用者与被调用者，评估修改该符号时可能影响的
//       代码范围（双向影响面分析），便于评估改动风险。
// 参数：
//   - symbol（必填）：目标符号名称
//   - max_depth（可选）：影响面遍历的最大深度，默认 5
// 用途：修改代码前评估改动波及范围，辅助制定重构/变更方案
class CodeGraphImpactTool : public XXToolBase {
protected:

    std::shared_ptr<agentxx::expand::CodeGraphManager> codegraph;

public:

    CodeGraphImpactTool(
        std::shared_ptr<agentxx::expand::CodeGraphManager> in_codegraph,
        std::weak_ptr<agentxx::agent::AgentContext>        in_agentContext
    );

    neograph::ChatTool get_definition() const override;

    asio::awaitable<std::string> execute_async(const neograph::json& arguments) override;
};

// 索引状态查询 tool（agentxx_codegraph_status）
// 功能：查询当前 codegraph 代码索引的状态，返回总节点数、总边数、
//       已索引文件数、循环依赖数等统计信息。
// 参数：无
// 用途：检查代码图谱索引是否已建立及其规模，决定是否需要先执行索引
class CodeGraphStatusTool : public XXToolBase {
protected:

    std::shared_ptr<agentxx::expand::CodeGraphManager> codegraph;

public:

    CodeGraphStatusTool(
        std::shared_ptr<agentxx::expand::CodeGraphManager> in_codegraph,
        std::weak_ptr<agentxx::agent::AgentContext>        in_agentContext
    );

    neograph::ChatTool get_definition() const override;

    asio::awaitable<std::string> execute_async(const neograph::json& arguments) override;
};

// 代码索引 tool（agentxx_codegraph_index）
// 功能：对指定目录建立/更新代码索引，支持增量索引；索引数据是其他
//       codegraph 系列 tool 查询的基础，使用前需先建立索引。
// 参数：
//   - path（必填）：待索引的目录路径
//   - incremental（可选）：是否增量索引（仅处理变更文件），默认 true
// 用途：初始化或更新代码图谱数据，返回索引后的节点/边/文件统计
class CodeGraphIndexTool : public XXToolBase {
protected:

    std::shared_ptr<agentxx::expand::CodeGraphManager> codegraph;

public:

    CodeGraphIndexTool(
        std::shared_ptr<agentxx::expand::CodeGraphManager> in_codegraph,
        std::weak_ptr<agentxx::agent::AgentContext>        in_agentContext
    );

    neograph::ChatTool get_definition() const override;

    asio::awaitable<std::string> execute_async(const neograph::json& arguments) override;
};

// 调用路径查询 tool（agentxx_codegraph_path）
// 功能：查找两个符号之间经由调用/依赖关系连通的路径，返回路径上的符号
//       序列（含类型、文件、行号）及路径深度。
// 参数：
//   - from（必填）：起点符号名称
//   - to（必填）：终点符号名称
//   - max_depth（可选）：路径搜索的最大深度，默认 10
// 用途：分析两个符号之间是否存在调用链以及如何相互关联
class CodeGraphPathTool : public XXToolBase {
protected:

    std::shared_ptr<agentxx::expand::CodeGraphManager> codegraph;

public:

    CodeGraphPathTool(
        std::shared_ptr<agentxx::expand::CodeGraphManager> in_codegraph,
        std::weak_ptr<agentxx::agent::AgentContext>        in_agentContext
    );

    neograph::ChatTool get_definition() const override;

    asio::awaitable<std::string> execute_async(const neograph::json& arguments) override;
};

} // namespace tools
} // namespace agentxx

#endif
