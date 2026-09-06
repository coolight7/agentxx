# 插件 Client 端事件处理与声明式交互重构方案

> 状态: 设计提案 (待评审)  
> 目标: 彻底消除 TUI 核心层对特定插件 (如 Planning、CodeGraph 等) 的特化分支与硬编码交互逻辑，建立一套纯粹由插件声明 UI、驱动事件回调、绑定函数执行的通用 Client 端交互架构。

---

## 1. 背景与核心诉求

### 1.1 现状与痛点

当前 Agentxx 的插件系统在 Client 端已经实现了初步的**声明式 UI 渲染**：
- 插件通过 `AgentxxClientUiIface` 注册面板 (`register_panel`)、Info 栏段落 (`register_info_section`) 或工具装饰 (`update_tool_decor`)。
- 渲染内容以统一的 JSON 数据传递 (`{"items": [{"kind": "text", ...}, {"kind": "button", ...}]}`)。
- TUI 具备通用的 `appendPluginItems` 负责把 JSON 翻译为 FTXUI 的 `Element`。

然而，当前的交互层存在**渲染与行为脱节**的核心缺陷：

| 交互维度 | 现状处理方式 | 核心缺陷与隐患 |
| :--- | :--- | :--- |
| **按钮展示** | 插件在 JSON 声明 `{"kind":"button","label":"..."}` | 仅具备视觉样式，无法携带交互意图 |
| **点击响应** | 宿主偷看特定字段 (如 `mermaid`) 或特化 ID (如 `agentxx_planning.plan`) 挂载命中区域 | 宿主产生插件特化分支，反向侵入核心层；新插件无法扩展交互 |
| **逻辑执行** | 宿主在 `agent_tui.cpp` 写死点击动作 (如写死打开状态图弹窗) | 插件无法在点击时执行自身 C/C++ 业务逻辑、无法调用后端能力 |
| **生命周期** | 按钮的 `ftxui::Box` 散落在 TUI 成员变量中单独维护 | 状态脆弱，缺乏通用帧级拾取器，难以支持动态列表与多按钮场景 |

### 1.2 核心诉求

1. **宿主彻底通用化 (Zero Specialization)**：
   TUI 核心层退化为纯粹的“渲染引擎 + 事件拾取派发器”，不包含任何插件名称、业务意图或特定弹窗的硬编码。
2. **真正的插件交互闭环 (True Plugin Interactivity)**：
   插件应当能够在声明按钮的同时，将点击事件绑定到插件实例自身的函数/闭包执行；插件可以在回调中发起网络请求、调用 Agent 工具、代发用户消息、更新自身 UI 或调用宿主基础能力。
3. **解决声明式 JSON 与函数执行的关联难点**：
   既要保持 C ABI 的安全与跨边界解耦，又要让插件开发者能够以现代 C++ 的直观方式（如 Lambda 闭包）编写交互逻辑。

---

## 2. 核心难点与约束分析

在 C++ 插件架构中，将纯声明式 JSON 与动态函数执行关联起来，存在以下关键技术约束：

### 2.1 C ABI 边界与多实例三铁律

- **类型安全边界**：插件由动态库 (`.so` / `.dll`) 承载，宿主与插件跨边界仅能传递 C 基础类型、定长结构体及纯 C 函数指针。禁止跨边界传递 `std::function`、C++ 类实例或依赖特定编译器虚表的类型。
- **多实例隔离**：同一进程内可能加载同一动态库的多个并发实例（多实例三铁律）。按钮回调不能依赖全局或函数静态变量，必须能够精确还原当前触发实例的上下文指针 (`user_data`)。
- **不可序列化性**：内存函数指针无法安全序列化为 JSON 文本（存在 ASLR 随机化、野指针攻击以及悬挂指针风险）。

### 2.2 FTXUI 渲染机制与屏幕坐标动态性

- **每帧动态拾取**：FTXUI 没有类似 DOM 的持久化树节点，界面每帧通过 `reflect(box)` 动态拾取屏幕绝对像素坐标。终端缩放、历史滚动、列表伸缩均会导致按钮坐标每帧变动。
- **事件与渲染生命周期脱节**：
  - 渲染发生在 `OnRender` 阶段（UI 线程）。
  - 点击发生在 `OnEvent` 阶段（UI 线程）。
  - 插件业务执行通常需要调度到 **Client IO 线程**，以保证与网络 IO、会话状态的同步无锁安全。
- **悬挂点击与 UAF (Use-After-Free) 隐患**：若用户点击屏幕的瞬间，插件正在异步推送全新 JSON（已删除旧按钮），拾取器必须防止触发已被释放的回调上下文。

---

## 3. 总体架构设计：Action-Driven 声明式交互模型

为了突破上述难点，设计采用 **Action-Driven（动作驱动）的三层混合交互模型**：

```
┌─────────────────────────────────────────────────────────────┐
│                    插件层 (C++ SDK / Kit)                    │
│   kit.button("Graph", [this](auto& args){ openRoadmap(); }) │
└──────────────────────────────┬──────────────────────────────┘
                               │  1. 自动生成 action_id
                               │  2. 本地实例保存 map<action_id, Lambda>
                               ▼
┌─────────────────────────────────────────────────────────────┐
│                    C ABI 边界 (纯数据与 C 函数)               │
│   JSON: {"kind":"button", "action_id":"act_1", ...}         │
│   C 回调: on_action(const AgentxxUiActionContext*, ud)       │
└──────────────────────────────┬──────────────────────────────┘
                               │
                               ▼
┌─────────────────────────────────────────────────────────────┐
│                  宿主层 (ClientPluginManager & TUI)          │
│   1. OnRender: 拾取按钮 Box -> HitTargetRegistry            │
│   2. OnEvent : 点击命中测试 -> postToIo -> 派发插件回调        │
│   3. Host Cap: 插件回调中反向请求宿主弹窗/Toast能力          │
└─────────────────────────────────────────────────────────────┘
```

### 3.1 架构分层职责

1. **L1 - C ABI 规范层 (ABI Specification)**：
   - 规定无状态的 `action_id` 协议与通用参数 `action_args`。
   - 定义统一的动作分发回调签名 `AgentxxUiActionFn`。
   - 扩展 UI 接口表，提供宿主通用基础 UI 能力（如模态弹窗、Toast 等）。
2. **L2 - 宿主引擎层 (TUI & ClientPluginManager)**：
   - **渲染器**：通用解析 JSON 中的 `button` / `action`，并自动生成帧级命中目标 (`HitTarget`)。
   - **拾取器**：在 FTXUI 鼠标事件中做统一几何拾取（Hit-Testing），识别点击项。
   - **派发器**：安全跨线程将点击动作投递回 Client IO 线程并调用对应插件回调。
3. **L3 - 插件 SDK 封装层 (Plugin Kit)**：
   - 为插件开发者提供轻量级 Controller，将 `action_id` 映射封装为现代 C++ Lambda，消除手写分支代码。

---

## 4. 接口设计与 ABI 规范 (API v1 增量)

在 `agent/lib/include/agentxx/plugin/api/client_plugin_api.h` 中进行 ABI 扩展。遵循 API v1 规范（8 字节对齐、明确调用约定、结构体指针入参出参）。

### 4.1 动作上下文与回调函数定义

```c
/// 动作触发上下文 (宿主传给插件的点击现场数据)
typedef struct AgentxxUiActionContext {
    int32_t                 version;     ///< 必须 == 1
    uint32_t                _reserved;   ///< 8 字节对齐
    AgentxxPluginStringView section_id;  ///< 所属段落/面板/装饰 ID
    AgentxxPluginStringView action_id;   ///< 按钮声明的 action_id
    AgentxxPluginStringView action_args; ///< 按钮携带的参数 JSON (可选, 空串表示无参数)
} AgentxxUiActionContext;

/// 动作执行回调函数签名 (由宿主在 Client IO 线程调用)
typedef void(AGENTXX_PLUGIN_CALL *AgentxxUiActionFn)(
    const AgentxxUiActionContext* ctx,
    void*                         user_data
);
```

### 4.2 注册规范结构体 (取代原本无结构的 propsJson)

为避免破坏旧接口指针布局，保持向后兼容性，对 `register_info_section` 和 `register_panel` 引入带 Spec 扩展的注册方式，或将现有 `propsJson` 赋予标准化 schema：

**方案选择：通过 propsJson 声明动作能力（轻量且完全向下兼容）**

无需更改现有的函数指针签名，插件在 `register_info_section` 或 `register_panel` 时传递的 `propsJson` 中声明支持的动作路由：

```c
/* 在 AgentxxClientUiIface 中扩展通用事件与能力表 */

/// 通用模态弹窗类型
typedef enum AgentxxModalType {
    AGENTXX_MODAL_MERMAID = 0,  ///< Mermaid 状态图/架构图全屏弹窗
    AGENTXX_MODAL_TEXT    = 1,  ///< 纯文本/Markdown 滚动查看弹窗
    AGENTXX_MODAL_DIFF    = 2,  ///< 代码 Diff 差异对比弹窗
    AGENTXX_MODAL_CUSTOM  = 3,  ///< 预留自定义组件弹窗
} AgentxxModalType;

/// 弹窗请求参数
typedef struct AgentxxModalSpec {
    int32_t                 version;     ///< 必须 == 1
    int32_t                 type;        ///< 见 AgentxxModalType
    AgentxxPluginStringView title;       ///< 弹窗标题
    AgentxxPluginStringView payload;     ///< 弹窗内容 (Mermaid源码/文本内容/Diff JSON)
    AgentxxPluginStringView extra_json;  ///< 扩展参数 (主题、宽高等)
} AgentxxModalSpec;

/* 在 AgentxxClientUiIface (升级至 VERSION 3) 新增通用操作接口 */
typedef struct AgentxxClientUiIface {
    int32_t version; ///< == 3
    uint32_t _reserved;
    
    // ... 保持既有状态栏、面板、Info段落、命令接口不变 ...

    /* ---- 通用交互与模态能力 (v3 新增) ---- */
    
    /// 注册段落/面板的交互动作监听器 (为指定 section/panel 绑定统一的 action 回调)
    int32_t(AGENTXX_PLUGIN_CALL* bind_action_handler)(
        const AgentxxPluginHost*       host,
        const AgentxxPluginStringView* target_id,  ///< section_id 或 panel_id
        AgentxxUiActionFn              on_action,  ///< 动作触发回调
        void*                          user_data   ///< 实例上下文
    );

    /// 弹出通用宿主模态框 (如 Mermaid 状态图、文本详情等)
    int32_t(AGENTXX_PLUGIN_CALL* open_modal)(
        const AgentxxPluginHost*  host,
        const AgentxxModalSpec*   spec
    );

    /// 关闭当前打开的模态框
    void(AGENTXX_PLUGIN_CALL* close_modal)(
        const AgentxxPluginHost* host
    );
} AgentxxClientUiIface;
```

---

## 5. 声明式 JSON Schema 规范

无论是 Info 栏段落、侧边栏面板、还是消息体展开装饰，所有的 `items` 统一遵循增强后的按钮 Schema：

### 5.1 按钮元素定义

```json
{
  "kind": "button",
  "label": "[Graph]",
  "prefix": "|- ",
  "action_id": "planning.open_graph",
  "args": {
    "view_mode": "full",
    "step_id": "phase_1"
  },
  "role": "normal"
}
```

字段说明：
- `kind`: 固定为 `"button"`。
- `label`: 按钮显示的文字标签（如 `"[Graph]"`、`"Retry"`）。
- `prefix`: 可选的前缀字符串（如 `"|- "`）。当配置前缀时，渲染器自动将前缀与按钮以同一行展示，保证对齐。
- `action_id`: **关键字段**。用户点击该按钮时派发的动作唯一标识符。
- `args`: 可选的 JSON 对象。插件可以在声明时塞入业务上下文，点击时宿主原样透传回回调函数。
- `role`: 可选视觉等级：`"normal"`（默认按钮背景）、`"accent"`（强调色）、`"danger"`（危险/报错色）。

### 5.2 兼容“行内文本 + 按钮”的紧凑语法

除了独立的 `button` 项外，很多树状视图（如规划列表）需要前导文本后紧跟按钮。
通用渲染器支持两种表达方式（插件可任选其一，效果完全等价）：
1. **隐式同行合并**：连续的 `{"kind":"text", "text":"|- "}` 紧跟 `{"kind":"button", "label":"[Graph]"}`。
2. **显式前缀属性**：单个按钮项携带 `"prefix": "|- "`。

---

## 6. 宿主 TUI 侧的通用拾取与派发机制

### 6.1 帧级命中目标注册表 (HitTargetRegistry)

TUI 不需要针对任何特定插件存储 `Box`（如不再需要 `planGraphButtonBox_`、`failedViewButtonBox_` 等）。改为每帧统一维护动态拾取列表：

```cpp
// 帧内可点击目标条目
struct UiHitTarget {
    ftxui::Box        box;         // 屏幕绝对渲染包围盒 (由 reflect 填充)
    std::string       targetId;    // 所属 section_id 或 panel_id
    std::string       actionId;    // 声明的 action_id
    neograph::json    args;        // 声明时携带的参数
    AgentxxUiActionFn callback;    // 注册时绑定的 C 回调函数
    void*             userData;    // 插件实例上下文指针
};
```

### 6.2 渲染生命周期管线 (Render Pipeline)

1. **每帧渲染前清空**：
   在 `renderInfoSidebar()` / `renderPluginPanel()` 入口：
   ```cpp
   hitTargets_.clear();
   ```
2. **渲染元素并挂载 Reflect**：
   在通用 `appendPluginItems` 处理 `kind == "button"` 时：
   - 检查该按钮是否声明了 `action_id` 且当前 section/panel 绑定了有效回调。
   - 若是，向 `hitTargets_` 追加一项，并将当前 FTXUI 元素的包围盒通过 `reflect(target.box)` 关联：
     ```cpp
     if (!actionId.empty() && actionFn) {
         auto& target = hitTargets_.emplace_back();
         target.targetId = sec.id;
         target.actionId = actionId;
         target.args     = it.value("args", neograph::json::object());
         target.callback = actionFn;
         target.userData = actionUserData;
         btnElement      = btnElement | reflect(target.box);
     }
     ```

### 6.3 鼠标事件拾取与无锁跨线程派发

在 `agent_tui.cpp` 的 `OnEvent` 鼠标处理逻辑中：

```cpp
if (mouse.button == Mouse::Left && mouse.motion == Mouse::Released) {
    // 统一命中测试
    for (const auto& target : hitTargets_) {
        if (target.box.Contain(mouse.x, mouse.y)) {
            // 命中有效按钮 -> 异步派发至 Client IO 线程
            const auto targetId = target.targetId;
            const auto actionId = target.actionId;
            const auto argsJson = target.args.dump();
            const auto cb       = target.callback;
            void*      ud       = target.userData;

            clientMgr_->postToIo([=]() {
                AgentxxUiActionContext actCtx{
                    .version     = 1,
                    ._reserved   = 0,
                    .section_id  = PluginStringView::from(targetId),
                    .action_id   = PluginStringView::from(actionId),
                    .action_args = PluginStringView::from(argsJson),
                };
                // 安全调用插件回调
                cb(&actCtx, ud);
            });
            return true; // 拦截事件，避免穿透
        }
    }
}
```

**安全性设计说明**：
- 点击事件在 UI 线程发生后，仅拷贝字符串与指针参数，通过线程池安全投递到 Client IO 线程。
- 插件的 `callback` 执行在 Client IO 线程，与插件的网络通信、事件订阅处于同一线程上下文，不存在多线程数据竞态。

---

## 7. 插件 SDK / Kit 封装：现代 C++ 交互体验

为了避免插件编写繁琐的 C 风格字符串对比，在 `plugin_kit.h` 提供轻量级交互控制器封装：

```cpp
namespace agentxx::kit {

class ActionController {
public:
    using ActionHandler = std::function<void(const neograph::json& args)>;

    /// 绑定命名动作处理函数
    void on(std::string actionId, ActionHandler handler) {
        handlers_[std::move(actionId)] = std::move(handler);
    }

    /// 便捷生成按钮 JSON 并自动绑定无参 Lambda
    neograph::json makeButton(
        std::string label,
        std::function<void()> onClick,
        std::string prefix = ""
    ) {
        std::string actId = fmt::format("act_{}", ++counter_);
        handlers_[actId]  = [fn = std::move(onClick)](const neograph::json&) {
            fn();
        };
        neograph::json btn;
        btn["kind"]      = "button";
        btn["label"]     = std::move(label);
        btn["action_id"] = std::move(actId);
        if (!prefix.empty()) {
            btn["prefix"] = std::move(prefix);
        }
        return btn;
    }

    /// C ABI 派发入口 (由 plugin_kit 自动连接到宿主 bind_action_handler)
    static void AGENTXX_PLUGIN_CALL dispatch(const AgentxxUiActionContext* ctx, void* ud) {
        auto* self = static_cast<ActionController*>(ud);
        if (!self || !ctx) return;
        std::string actId(ctx->action_id.data, ctx->action_id.size);
        if (auto it = self->handlers_.find(actId); it != self->handlers_.end()) {
            neograph::json args = neograph::json::object();
            if (ctx->action_args.data && ctx->action_args.size > 0) {
                try {
                    args = neograph::json::parse(std::string_view(ctx->action_args.data, ctx->action_args.size));
                } catch (...) {}
            }
            it->second(args);
        }
    }

private:
    std::unordered_map<std::string, ActionHandler> handlers_;
    uint64_t counter_ = 0;
};

} // namespace agentxx::kit
```

---

## 8. 重构案例推导：Planning Graph 按钮的彻底重塑

重构完成后，以 `agentxx_planning` 插件为例，展现架构演进的质变：

### 8.1 重构前 vs 重构后时序对比

```mermaid
sequenceDiagram
    autonumber
    Note over PlanningPlugin, TUI: 【重构前】特化耦合
    PlanningPlugin->>TUI: update_info_section(包含 roadmap 与硬编码字段)
    Note over TUI: TUI 特化检测: if sec.id == "agentxx_planning.plan"<br/>提取首个 button，强制塞入 titleRow 右侧
    User->>TUI: 鼠标点击 Plan 标题右侧
    Note over TUI: TUI 内部硬编码调 openMermaidDiagram<br/>(插件对此完全无感知)

    Note over PlanningPlugin, TUI: 【重构后】完全由插件自决驱动
    PlanningPlugin->>Kit: makeButton("[Graph]", [this]{ openModal(); }, "|- ")
    Kit->>PlanningPlugin: 返回通用 Button JSON (携带 action_id)
    PlanningPlugin->>TUI: update_info_section(通用 items JSON)
    Note over TUI: TUI 通用渲染器按声明正常同行渲染 "|- " + [Graph]<br/>reflect 挂载至通用的 HitTargetRegistry
    User->>TUI: 鼠标点击 [Graph] 按钮
    TUI->>PlanningPlugin: 派发 on_action("planning.open_graph", args)
    PlanningPlugin->>TUI: ui->open_modal(AGENTXX_MODAL_MERMAID, title, roadmap)
    Note over TUI: 宿主通用模态组件展示弹窗
```

### 8.2 插件端实现代码 (Clean & Idiomatic)

在 `agentxx_planning.cpp` 内部：

```cpp
// 1. 初始化阶段绑定段落处理器
ctx.ui->bind_action_handler(ctx.host, &secIdSv, &ActionController::dispatch, &ctx.actions);

// 2. 注册 Graph 按钮点击行为
ctx.actions.on("planning.open_graph", [&ctx](const neograph::json&) {
    AgentxxModalSpec spec{
        .version    = 1,
        .type       = AGENTXX_MODAL_MERMAID,
        .title      = PluginStringView::fromCstr("Planning Roadmap"),
        .payload    = PluginStringView::from(ctx.current_roadmap),
        .extra_json = PluginStringView::fromCstr("{}"),
    };
    ctx.ui->open_modal(ctx.host, &spec);
});

// 3. 构建渲染 items
if (!roadmap.empty()) {
    neograph::json btn;
    btn["kind"]      = "button";
    btn["prefix"]    = "|- ";
    btn["label"]     = "[Graph]";
    btn["action_id"] = "planning.open_graph";
    items.push_back(btn.dump());
}
```

---

## 9. 演进路线与实施计划

重构按三阶段平滑演进，不破坏现有功能：

### 阶段 1：ABI 接口扩展与宿主基础能力 (Core Extension)
1. 在 `client_plugin_api.h` 定义 `AgentxxUiActionContext`、`AgentxxUiActionFn` 与 `AgentxxModalSpec`。
2. 升级 `AgentxxClientUiIface` 版本至 3，增加 `bind_action_handler` 与 `open_modal` 接口定义。
3. 在 `ClientPluginManager` 中实现动作绑定表的注册、注销与生命周期清理。

### 阶段 2：宿主通用拾取与通用弹窗 (TUI Universal Dispatch)
1. 在 `TUIClientAgentIO` 引入 `std::vector<UiHitTarget> hitTargets_` 通用拾取管道。
2. 重构 `appendPluginItems`，使其自动为带 `action_id` 的按钮执行通用 reflect 挂载。
3. 在 `TUIClientAgentIO` 实现通用的 `openModal(spec)`，取代旧有专用的 `openMermaidDiagram`。
4. 移除 `agent_tui.cpp` 中所有针对 `planGraphButtonBox_`、`failedViewButtonBox_` 的专用点击分支，统一接入 `hitTargets_` 派发。

### 阶段 3：插件 SDK 适配与业务迁移 (Migration)
1. 在 `plugin_kit.h` 增加 `ActionController` 便捷封装。
2. 迁移 `agentxx_planning` 插件采用标准的 `action_id` 与 `open_modal` 模式。
3. 补充插件按钮点击、跨线程派发及模态框唤起的自动化单元测试。
