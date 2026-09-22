#pragma once

/// 客户端 UI 组件描述 schema (`agentxx.ui.item`)
///
/// 背景: 插件面板 / Info 段落 / 工具消息装饰 / 通用 overlay / 中断描述此前都用
/// "items JSON 数组"表达界面, 但可用的组件只有文本、按钮、进度条等少数几种,
/// 布局 (横排/分组/折叠) 与结构化展示 (表格/树/键值/图表) 完全缺失, 且渲染实现
/// 分散在多个接入点各写一份。
///
/// 本文件是该 schema 的**数据模型与解析/校验/序列化实现**:
/// - 所有前端 (TUI / CLI / FFI / 日志) 共用同一份描述解析与纯文本降级
/// - 不依赖任何 UI 库 (FTXUI 等只在具体渲染实现里出现), 因此插件与 agent 侧
///   代码都能复用
/// - 新组件只扩展本 schema 与渲染实现, 不改插件接口表 (数据层扩展)
///
/// 组件清单 (kind):
/// - 文本类: `text` / `markdown` / `diff` / `separator` / `gap`
/// - 结构类: `row` / `box` / `collapse` / `table` / `tree` / `kv`
/// - 图表类: `sparkline` / `meter` / `progress`(旧写法, 等同 meter) / `badge` / `diagram`
/// - 交互类: `button`(别名 `action`) / `control` / `submit`
/// - 预留:   `canvas`(完全自绘; 本版只解析与降级) / `custom`(派发到内置组件)
///
/// 未知 kind 不使整份描述失效: 解析结果标记 `known = false`, 渲染时输出
/// `fallback` 文本 (无 fallback 则跳过), 保证向前兼容。
#include "agentxx/ui/text_width.h"
#include "utilxx_base/json.h"
#include <cstddef>
#include <string>
#include <vector>

namespace agentxx {
namespace ui {

/// 表格列声明
struct TableColumn {
    std::string title;
    /// 单元格水平对齐: left / center / right
    std::string align = "left";
    /// 固定列宽 (显示列; 0 = 由内容或剩余宽度决定)
    int width = 0;
    /// 是否占据剩余宽度 (多列同时声明时按顺序分配)
    bool flex = false;
    /// 列默认文字色 (空 = normal)
    std::string color;
};

/// 表格单元格 (字符串单元格解析后也会归一化为本结构)
struct TableCell {
    std::string       text;
    std::string       color;
    std::string       action;
    utilxx_base::Json args;
};

/// 树节点
struct TreeNode {
    std::string           label;
    std::string           color;
    std::string           action;
    utilxx_base::Json     args;
    std::vector<TreeNode> children;

    /// 展开后的节点总数 (含自身; 用于数量上限判断)
    size_t count() const;
};

/// 键值对项
struct KeyValuePair {
    std::string key;
    std::string value;
    std::string keyColor;
    std::string valueColor;
};

/// 计量条阈值 (达到该值时使用对应颜色; 由高到低匹配首个满足项)
struct MeterThreshold {
    double      at = 0.0;
    std::string color;
};

/// 控件候选项 (control: buttons / select)
struct ControlOption {
    /// 选中时写入结果的原始值 (缺失按空字符串)
    utilxx_base::Json value;
    /// 显示标签
    std::string label;
    /// 文本色覆盖 (空 = 按控件样式)
    std::string color;
};

/// 解析上限 (越界按"截断/丢弃"处理, 不使整份描述失效)
struct ParseLimits {
    /// 嵌套深度上限 (row/box/collapse 等容器)
    int    maxDepth       = 8;
    /// 单层 items 数组元素数上限
    size_t maxItems       = 512;
    /// 表格行数上限
    size_t maxTableRows   = 512;
    /// 表格列数上限
    int    maxTableColumns = 16;
    /// 树节点数上限 (展开后)
    size_t maxTreeNodes   = 1024;
    /// 迷你趋势图数据点上限 (超出按桶聚合前先截断)
    size_t maxDataPoints  = 4096;
    /// 单项文本字节上限
    size_t maxTextBytes   = 64 * 1024;
};

/// 默认解析上限
inline constexpr ParseLimits kDefaultParseLimits{};

/// 组件项 (schema 的规范化形态; 各 kind 只使用自己相关的字段)
struct Item {
    /// 组件类型 (缺省 `text`)
    std::string kind = "text";
    /// 组件标识 (表单控件必填; 其他可选, 用于状态保持与诊断)
    std::string id;
    /// 左侧缩进空格数 (与渲染上下文的基础缩进叠加)
    int indent = 0;
    /// 文字色名 (语义色: normal/hint/accent/error/tool/thinking/user/assistant/system/title)
    std::string color = "normal";
    bool        bold  = false;
    bool        dim   = false;
    /// 是否按可用宽度折行 (文本类缺省 true, 结构化类忽略)
    bool wrap = false;
    /// 条件显示表达式 (预留字段: 当前仅解析/往返保留, 渲染不消费)
    ///
    /// 规划语义: 由宿主状态决定是否渲染 (如 `"expanded"` = 所在折叠分组已展开)。
    /// 保留字段可让插件先按未来 schema 推送而不丢内容; 宿主侧不识别时按"总是显示"
    /// 处理 (不影响其余字段)。
    std::string when;
    /// 不被当前宿主支持时的降级文本
    std::string fallback;
    /// 点击派发动作 id (可点组件)
    std::string action;
    /// 点击派发参数 (原样回传)
    utilxx_base::Json args;
    /// 解析结果是否被识别 (false = 未知 kind / 解析失败 → 走 fallback)
    bool known = true;

    // ---- text / markdown ----
    /// 文本内容 (markdown 为源码)
    std::string text;

    // ---- gap ----
    /// 空行数
    int lines = 1;

    // ---- button (别名 action) ----
    /// 按钮文字
    std::string label;
    /// 同行前导文本 (空 = 无)
    std::string prefix;
    /// 按钮样式: solid(默认, 反色块) / text(纯文字按钮, 加下划线强调)
    std::string style = "solid";

    // ---- meter / progress ----
    /// 当前值
    double value = 0.0;
    /// 满值 (>0; 0 时按 100 处理)
    double total = 100.0;
    /// 计量条宽度 (显示列; 0 = 使用默认 20 列)
    int width = 0;
    /// 数值单位 (如 "%"; 显示在数值后)
    std::string unit;
    /// 阈值配色 (由高到低匹配)
    std::vector<MeterThreshold> thresholds;
    /// 是否显示数值文本 (缺省按 kind 决定: meter 显示, sparkline 由 showLast 决定)
    bool showValue = false;

    // ---- sparkline ----
    /// 数据点
    std::vector<double> data;
    /// 图形高度 (行; 1 = 单行块字符)
    int height = 1;
    /// 绘制风格: block(▁▂▃▄▅▆▇█) / bar(▏▎▍▌▋▊▉█) / auto
    std::string sparkStyle = "block";
    bool        hasMin     = false;
    bool        hasMax     = false;
    double      minValue   = 0.0;
    double      maxValue   = 0.0;
    /// 是否在图形后追加最新值
    bool showLast = false;
    /// 按值分档的配色 (由低到高; 空 = 单色)
    std::vector<std::string> colors;

    // ---- row ----
    /// 列间距 (空格数)
    int gap = 0;
    /// 行内对齐: left / center / right / stretch
    std::string align = "left";
    /// 子项 (row/box/collapse 使用)
    std::vector<Item> items;
    /// 列宽权重: >0 固定列数, `flex = true` 占据剩余宽度
    int  columnWidth = 0;
    bool columnFlex  = false;

    // ---- box ----
    /// 标题 (空 = 无标题)
    std::string title;
    /// 边框风格: none / square / round / light
    std::string border = "none";
    /// 标题文字色 (空 = accent)
    std::string titleColor;
    /// 内边距 (行数)
    int pad = 0;

    // ---- collapse ----
    /// 初始展开状态 (实际展开状态由宿主维护; 缺省展开)
    bool expanded = true;

    // ---- table ----
    /// 是否渲染表头行
    bool                      header = false;
    std::vector<TableColumn>  columns;
    std::vector<std::vector<TableCell>> rows;

    // ---- tree ----
    /// 是否绘制连接线
    bool                  connector = true;
    std::vector<TreeNode> nodes;

    // ---- kv ----
    /// 键值分隔符
    std::string                sep = " : ";
    std::vector<KeyValuePair>  pairs;
    /// 键列宽度 (显示列; 0 = 按最长键自适应)
    int keyWidth = 0;

    // ---- diff ----
    std::string path;
    std::string oldStr;
    std::string newStr;

    // ---- diagram ----
    std::string mermaid;

    // ---- control (交互控件; 语义同中断描述) ----
    /// 控件形态: buttons / select / text / number / checkbox
    std::string control;
    /// 控件标签 (渲染在控件上方; 空 = 不渲染标签行)
    std::string controlLabel;
    /// 控件说明 (渲染在标签下方; 空 = 不渲染)
    std::string help;
    /// 候选项 (buttons / select)
    std::vector<ControlOption> options;
    /// 缺省值 (checkbox = 布尔; number = 数值; 其余 = 候选项值/文本)
    utilxx_base::Json defaultValue;
    /// buttons: 点击即选中并提交整份表单
    bool commitOnPick = false;
    /// number: 仅接受整数
    bool   integer    = false;
    bool   hasNumMin  = false;
    double numMin     = 0.0;
    bool   hasNumMax  = false;
    double numMax     = 0.0;
    double step       = 1.0;
    /// text: 多行输入 (预留字段; 当前按单行渲染)
    bool multiline = false;

    // ---- submit ----
    /// 取消按钮标签 (空 = 前端按语言取默认文案)
    std::string cancelLabel;

    // ---- custom / canvas ----
    /// 派发到的内置组件名 (custom: 空 = 按 props.items 走组件树)
    std::string component;
    /// 组件属性 (custom: 原样作为该组件的参数; 也可含 items)
    utilxx_base::Json props;
    /// canvas 原始描述 (本版原样保留, 保证往返不丢; 渲染走 fallback)
    utilxx_base::Json canvas;

    /// 是否含可交互内容 (控件 / 提交行 / 带动作的可点组件)
    bool interactive() const;
};

/// 单条 JSON → 组件项 (使用默认上限, 顶层)
/// - 非对象输入返回已知性为 false 的空项 (kind = text, 无文本)
/// - 未知 kind 返回 `known = false` 的项, 保留 `fallback`
Item parseItem(const utilxx_base::Json& json);

/// 单条 JSON → 组件项 (显式上限与嵌套层数; 递归解析容器类组件时使用)
Item parseItem(const utilxx_base::Json& json, const ParseLimits& limits, int depth);

/// JSON 数组 → 组件项列表 (非数组输入返回空列表)
std::vector<Item>
    parseItems(const utilxx_base::Json& json, const ParseLimits& limits = kDefaultParseLimits);

/// `{"items":[...]}` 或裸数组 → 组件项列表 (两种写法都接受)
std::vector<Item> parseItemList(
    const utilxx_base::Json& json,
    const ParseLimits&       limits = kDefaultParseLimits
);

/// 组件项 → JSON (字段缺省值不输出, 保证往返简洁)
utilxx_base::Json dumpItem(const Item& item);
/// 组件项列表 → JSON 数组
utilxx_base::Json dumpItems(const std::vector<Item>& items);

/// 文本长度截断 (按字节上限截断, 不切断 UTF-8 码点)
std::string clampText(std::string_view text, size_t maxBytes);

/// 组件树 → 纯文本 (行式前端 / 日志 / CLI 降级)
/// - table → 列对齐文本 + 分隔线; tree → 连接线前缀; kv → `k: v`
/// - sparkline → 末值 + 简图; meter → `[####----] 72%`
/// - control → `标签: 候选项/默认值 (形态)`; submit 不输出
/// - row → 子项以 ` | ` 连接; box → 标题行 + 内容
/// - `args`:
///     - [width] 折行宽度 (<=0 不折行)
std::string plainText(const std::vector<Item>& items, int width = 0);

/// 组件树 JSON (数组或 `{"items":[...]}`) → 纯文本
std::string plainText(const utilxx_base::Json& json, int width = 0);

} // namespace ui
} // namespace agentxx
