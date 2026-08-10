#pragma once

#include "ftxui/dom/elements.hpp"
#include "ftxui/screen/color.hpp"
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace agentxx {
namespace client {
namespace tui {

/// Mermaid stateDiagram-v2 子集解析结果
///
/// 支持语法 (容错: 未知/不支持语法安全忽略, 解析不失败):
///   stateDiagram / stateDiagram-v2          头
///   direction LR | TB (RL/BT 归一化为 LR/TB) 布局方向
///   state "label" as id / state id / state "label"   状态声明
///   [*]                                       起始/结束伪状态
///   A --> B / A --> B: label                  转移
///   %% 注释行
/// 不支持 (忽略): note/复合状态体/条件转移等
struct MermaidStateNode {
    std::string id;               // 节点标识
    std::string label;            // 显示文本 (多行以 \n 分隔; 空 = 使用 id)
    bool        isPseudo = false; // [*] 起始/结束伪状态
};

struct MermaidStateEdge {
    size_t      from; // nodes 下标
    size_t      to;
    std::string label; // 边标签 (可为空)
};

struct MermaidStateDiagram {
    std::vector<MermaidStateNode> nodes;
    std::vector<MermaidStateEdge> edges;
    bool                          directionLR = false; // direction LR: 横向分层布局
};

/// 解析 mermaid stateDiagram-v2 源码
MermaidStateDiagram parseMermaidStateDiagram(std::string_view source);

/// 渲染状态图为 ftxui Element (分层网格布局 + 边带路由)
///
/// @param maxWidth     <=0 不限制; >0 时按层行宽预算截断标签 (必要时退回 TB 布局)
/// @param defaultColor 未显式着色单元的默认颜色;
///                     Color::Default 表示不施加装饰 (由外层装饰继承, 如消息正文整体着色)
/// @param nodeColor    按节点 id 提供颜色的回调 (如按状态后缀着色);
///                     返回 Color::Default 表示不着色
ftxui::Element renderMermaidStateDiagram(
    const MermaidStateDiagram&                           dg,
    int                                                  maxWidth     = 0,
    ftxui::Color                                         defaultColor = ftxui::Color::Default,
    const std::function<ftxui::Color(std::string_view)>& nodeColor    = nullptr
);

} // namespace tui
} // namespace client
} // namespace agentxx
