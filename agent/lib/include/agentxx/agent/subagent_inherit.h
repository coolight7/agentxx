#pragma once

namespace agentxx {
namespace agent {

/// 子代理从父配置继承哪些"重量级"资源
///
/// - 默认**全部关闭** = 轻量子代理 (现状行为): 只带内置/中间件工具,
///   不挂插件、不连 MCP、不注入父级 Skill/Memory/RAG
/// - 由委派调用方按需打开 (`AgentHost::makeSubagentConfig` 的参数):
///   `agentxx_subagent` 工具把全部打开, 让子代理与主会话拥有同一套工具;
///   上下文压缩等内部路径保持缺省
/// - 只影响"配置派生", 不影响子代理的隔离语义 (独立会话上下文、独立节点)
struct SubagentInherit {
    /// 插件挂载: 继承 `pluginMountPolicy` / `pluginMountList` /
    /// `pluginMountExclude` / `pluginMountArgs` (决定插件工具是否可用)
    bool plugins = false;
    /// MCP server 清单 (子代理会重新建立自己的连接)
    bool mcp = false;
    /// skill 目录 (`skillDirPaths`)
    bool skills = false;
    /// memory 文件 (`memoryFilePaths`)
    bool memory = false;
    /// RAG 文档目录 (`ragDocsPaths`)
    bool rag = false;

    /// 全部关闭 (= 轻量子代理, 与未加本参数时一致)
    static SubagentInherit none() { return SubagentInherit{}; }

    /// 全部打开 (= 与父 agent 加载同样的插件/工具/MCP/Skill/Memory)
    static SubagentInherit full() {
        SubagentInherit out;
        out.plugins = true;
        out.mcp     = true;
        out.skills  = true;
        out.memory  = true;
        out.rag     = true;
        return out;
    }

    /// 是否有任何一项要求继承 (日志/分支判断用)
    bool any() const { return plugins || mcp || skills || memory || rag; }

    /// 合并 (任一来源要求继承即继承)
    SubagentInherit operator|(const SubagentInherit& other) const {
        SubagentInherit out = *this;
        out.plugins         = out.plugins || other.plugins;
        out.mcp             = out.mcp || other.mcp;
        out.skills          = out.skills || other.skills;
        out.memory          = out.memory || other.memory;
        out.rag             = out.rag || other.rag;
        return out;
    }
};

}; // namespace agent
}; // namespace agentxx
