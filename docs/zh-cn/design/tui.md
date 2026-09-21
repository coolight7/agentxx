# TUI 实现与架构 (FTXUI)
> 相关文档: [index.md](index.md) (整体设计) · [plugins.md](plugins.md) (插件系统, 含插件 UI 扩展)

本文整理 client 端 TUI 的实现与分层: 先说明 FTXUI 的机制 (事件/渲染/坐标/命中),
再说明本项目在其上的分层与约定, 最后给出交互框架 (命中登记表 / 声明式条目列表)
的使用方法, 以及必须遵守的约束 (每帧重建、reflect 生命周期、模态阻塞)。

代码位置:
- 入口与主循环: [agent_tui.cpp](/agent/client/src/io/tui/agent_tui.cpp), [agent_tui.h](/agent/client/include/agentxx-client/io/tui/agent_tui.h)
- 交互框架: [framework/ui_hit.h](/agent/client/include/agentxx-client/io/tui/framework/ui_hit.h),
  [framework/ui_action_list.h](/agent/client/include/agentxx-client/io/tui/framework/ui_action_list.h)
- 组件: [components/](/agent/client/include/agentxx-client/io/tui/components/), 面性外框 [surface.h](/agent/client/include/agentxx-client/io/tui/surface.h)

---

## 1. FTXUI 机制摘要

### 1.1 两层结构: Element (DOM) 与 Component

| 层 | 类型 | 职责 |
|----|------|------|
| DOM | `ftxui::Element` (= `std::shared_ptr<Node>`) | 布局与绘制; 每帧重建 |
| 组件 | `ftxui::Component` (= `std::shared_ptr<ComponentBase>`) | 事件处理 + 焦点; 跨帧存活 |

关键点:

- **Element 是"每帧重建"的一次性对象**: 数据变化 -> 重建 Element 树 -> 布局 (`ComputeRequirement`/`SetBox`) -> 绘制 (`Render`)。
- **Component 的 `Render()` 每帧产出**新 Element, 事件经 `OnEvent()` 从根节点递归下发。
- `ComponentBase::Render()` 会把子元素包裹一层 `Wrapper` 节点, 用于把 `Active()`/`Focused()`
  (焦点态) 传给绘制层 (见 `src/ftxui/component/component.cpp`)。
- 组件树仅用于**事件路由与焦点**, 与渲染树是分开的: 用 `Renderer(child, lambda)` 时
  渲染完全由 `lambda` 决定, `child` 只负责事件 (本项目主界面即如此)。

### 1.2 事件路由

- `ComponentBase::OnEvent(event)`: 依次询问每个子组件, 某个子组件返回 `true` 即停止;
  全部返回 `false` 则事件未被处理。
- `Container::Vertical/Horizontal`: 键盘事件只给 `ActiveChild()`, 鼠标事件广播给所有子组件;
  容器自身处理 Up/Down (或 Left/Right) 切换选中子组件。
- `Container::Tab`: **只**把事件给当前选中的子组件 (隐藏页签收不到事件)。
- `Container::Stacked`: 渲染时 `dbox` 叠加, 事件按子组件顺序询问, 先处理者优先。
  本项目用它做"主界面事件路由器" (`messageList_ / sidebar_ / inputBar_ / statusBar_`)。
- `CatchEvent(child, fn)`: **先**调用 `fn`, 返回 `false` 时才继续给 `child`。
  本项目用它在组件树之前接管全局快捷键 (Ctrl+C / F2-F4 / F12)、鼠标拖选复制与
  shell 级按钮点击。
- `App::HandleTask` 在事件分发前把鼠标坐标从"终端绝对坐标"换算为"屏幕坐标"
  (减去 `cursor_x_/cursor_y_`), 并在 `handled == false` 时把左键事件转入**文本选择**
  (`HandleSelection`): 因此"未被任何组件处理的左键拖动"会成为屏幕选中;
  反之, 组件一旦返回 `true` 表示消费事件, 该次拖动不会产生选择。

### 1.3 坐标与命中检测

- `ftxui::Box{x_min, x_max, y_min, y_max}` 为**闭区间**的屏幕绝对坐标。
- `reflect(Box&)`: 一个装饰器; 布局 (`SetBox`) 时把该元素的区域写回 `Box`, 绘制时再与
  `screen.stencil` 求交 (`Box::Intersection`)。注意求交发生在**绘制**阶段: 只有本帧真正
  `Render()` 到的元素才会被收敛为空区域 —— 本项目自实现的滚动容器
  ([Scrollable](/agent/client/include/agentxx-client/io/tui/scrollable.h) /
  [LazyScrollable](/agent/client/include/agentxx-client/io/tui/lazy_scrollable.h))
  只为视口内的子项做定位与绘制, 视口外子项的 Box 会停在测量阶段写入的"测量用临时大框"
  (局部坐标, 与屏幕坐标部分重叠, 见 3.1)。
  因此**滚动容器内的子项命中不要用 `reflect`, 用容器输出的 `visibleBoxes()`**。
- 命中检测的常规写法是"组件持有 `Box` 成员 + `reflect(box_)` + `OnEvent` 里 `box_.Contain(x,y)`"
  (FTXUI 内置的 Button/Hoverable/Window 都是这样)。
  **局限**: `reflect` 只在元素参与布局时写回坐标 ——
  - 元素**未渲染**(条件分支跳过)时 Box 保留上一帧的值, 于是"已经不显示的按钮"仍在原位置命中;
  - 默认构造的 `ftxui::Box{}` 四个分量都是 0, `Contain(0, 0)` 为真, 它是**屏幕左上角**而不是"空区域"。
- `screen.CellAt(x, y)` 可读取单元格 (字符/前景/背景), 离屏测试用它断言配色与布局。

### 1.4 动画与帧

- 组件在 `OnAnimation(Params&)` 中推进动画, 并调用 `animation::RequestAnimationFrame()` 请求下一帧
  (由根组件沿组件树转发, 未 `Add()` 进树的组件收不到回调)。
- `App::RunOnce()` 无任务时不重绘; 本项目用 `Loop::RunOnceBlocking()` + `Event::Custom`
  显式请求重绘 (贴日志、delta 到达、状态变化等), 空闲时不烧 CPU。

### 1.5 弹窗与滚动

- 内置 `Modal(main, modal, bool*)` 只是 `dbox` 叠加, 不阻塞事件; 本项目使用自实现的
  [ModalContainer](/agent/client/include/agentxx-client/io/tui/framework/modal_container.h):
  打开弹窗时**只**把事件给弹窗 (不再下发给被遮挡的主界面), 渲染时也只渲染弹窗。
- 滚动容器为项目自实现 (FTXUI 的 `frame` 是"焦点跟随"式滚动, 不适用于会话记录):
  - [Scrollable](/agent/client/include/agentxx-client/io/tui/scrollable.h): 全量构建子项 + 视口局部布局/绘制
  - [LazyScrollable](/agent/client/include/agentxx-client/io/tui/lazy_scrollable.h): 懒构建 + LRU 缓存 (消息列表用),
    支持字节预算淘汰、头部插入锚定、`stickToBottom` 吸附

---

## 2. 本项目 TUI 分层架构

```
        client io 线程                                  UI 线程 (FTXUI Loop)
  ┌──────────────────────────┐               ┌───────────────────────────────────────────┐
  │ onDelta / onSync / ...   │  sharedState  │  CatchEvent (全局快捷键/拖选/shell 按钮)   │
  │  → TUISharedState (COW)  │ ───────────▶  │      ↓                                    │
  │  → enqueueUiAction(...)  │  每帧快照      │  ModalContainer                           │
  └──────────────────────────┘               │      ├─ 无弹窗: Renderer(Stacked) 主界面   │
                                             │      └─ 有弹窗: 弹窗组件 (全屏遮挡)         │
                                             └───────────────────────────────────────────┘
```

### 2.1 线程模型与状态

- **client io 线程**: 处理 wire 消息 (`onDelta/onSync/onPeerMessage`), 写 `TUISharedState` (COW:
  短锁 + `mutableState()` 深拷贝), 需要触碰组件树时经 `enqueueUiAction()` 投递到 UI 线程执行。
- **UI 线程**: 独占组件树; 每帧开头
  1. `ctx_.frameState = sharedState_.readSnapshot()` (本帧快照),
  2. `ctx_.refreshFrameSize()` (本帧终端尺寸, 同帧内所有组件读同一份, 且每帧只查一次终端),
  3. 执行待处理 UI 动作队列,
  4. `Loop::RunOnceBlocking()` (事件 + 渲染),
  5. 释放快照 (让 client 线程回到无拷贝的原地写路径)。
- 组件渲染只读 `ctx_.frameState`; 组件状态 (折叠集合、输入文本、弹窗选中项) 由组件自身持有。

### 2.2 组件清单

| 组件 | 文件 | 说明 |
|------|------|------|
| `MessageListComponent` | [components/message_list.h](/agent/client/include/agentxx-client/io/tui/components/message_list.h) | 消息列表 (LazyScrollable), 折叠/中断表单/附件/decor 按钮 |
| `InputComponent` | [components/input_bar.h](/agent/client/include/agentxx-client/io/tui/components/input_bar.h) | 输入框 + 附件托盘 + 待发队列行 |
| `StatusBarComponent` | [components/status_bar.h](/agent/client/include/agentxx-client/io/tui/components/status_bar.h) | 状态栏 (模型/上下文/插件项/会话/设置) |
| `SidebarComponent` | [components/sidebar.h](/agent/client/include/agentxx-client/io/tui/components/sidebar.h) | 侧边栏 (内容区 + tabs 列表 + 宽度拖拽) |
| 弹窗 | [components/overlays.h](/agent/client/include/agentxx-client/io/tui/components/overlays.h) | 模型/会话/设置/关于/待发队列/上下文/mermaid/text/diff/custom/文件选择 |
| `InterruptView` | [components/interrupt_view.h](/agent/client/include/agentxx-client/io/tui/components/interrupt_view.h) | 中断询问表单 (形态完全由描述数据决定) |
| `SpinnerComponent` | [components/spinner.h](/agent/client/include/agentxx-client/io/tui/components/spinner.h) | 帧序列加载动画 (动画等级低于门槛时静态降级) |
| `ui_components` | [ui_components.h](/agent/client/include/agentxx-client/io/tui/ui_components.h) | **组件渲染唯一实现**: 把 `agentxx.ui.item` 描述渲染为行模型 (元素 + 行数 + 元素内可命中区域) |

`ui_components` 是全部展示描述的唯一渲染实现: 侧边栏面板、Info 段落、工具消息装饰、
通用 overlay、中断描述内容块都经它渲染 (此前面板/Info/装饰各有一份 switch, 已收敛)。
新增一种组件只需改两处: `agentxx/ui/item.h` (字段与解析) 与 `ui_components.cpp` (渲染 + 测量);
行数估算走同一条渲染路径 (`measureItem`), 与真实布局高度一致 (见 3.1)。

弹窗统一用 [surface.h](/agent/client/include/agentxx-client/io/tui/surface.h) 的面性风格外框
(圆角 + 标题栏/内容区/底栏分区, 不画边框与分割线); 文案统一走
[tui_i18n.h](/agent/client/include/agentxx-client/io/tui/framework/tui_i18n.h) 的 `tr()/trf()`。

### 2.3 命中检测: 命中登记表

[framework/ui_hit.h](/agent/client/include/agentxx-client/io/tui/framework/ui_hit.h) 提供:

```c++
namespace agentxx::client {
/// 空命中区域 (x_min > x_max 且 y_min > y_max): 不要用 ftxui::Box{} 表示"无命中"
inline constexpr ftxui::Box kNoBox{0, -1, 0, -1};

/// 通用命中登记表: 帧首清空 + 渲染时登记 + 事件时查询
template <class Payload>
class UiHitRegistry {
    struct Entry { Payload payload; std::shared_ptr<ftxui::Box> box; };
    void            beginFrame();                       // 帧首清空 (必需)
    ftxui::Element  add(ftxui::Element, Args&&...);     // 登记并 reflect
    const Entry*    find(int x, int y) const;           // 坐标查询 (拖拽等)
    const Entry*    findClick(const ftxui::Mouse&) const; // 左键释放命中
    const std::vector<Entry>& entries() const;          // 遍历 (测试/调试)
};
using UiHitMap = UiHitRegistry<UiHitInfo>;              // payload = {id, arg}
}
```

**一条规则解决两类问题**: 每帧开头清空, **只有真正渲染出来的元素才登记**。
于是"未显示的按钮"既不占用点击区域, 也不需要清空成员 Box; 每个按钮也不必各占一个成员变量。

用法:

```c++
ftxui::Element XxxComponent::OnRender() {
    hits_.beginFrame();                                     // 帧首
    return ftxui::hbox({
        hits_.add(ftxui::text("设置"), std::string{kSettingsHitId}),   // 登记 + reflect
        hits_.add(ftxui::text("退出"), std::string{kQuitHitId}),
    });
}

bool XxxComponent::OnEvent(ftxui::Event event) {
    if (!event.is_mouse()) {
        return false;
    }
    const auto* hit = hits_.findClick(event.mouse());
    if (hit == nullptr) {
        return false;                                       // 未命中 -> 让其它组件处理
    }
    if (hit->payload == kSettingsHitId) {
        config_.onSettings();
    }
    return true;
}
```

载荷可以是任意类型 (如插件按钮归属 `UiHitTarget`、消息下标 `size_t`、条目归属 `TabHit`),
用 `UiHitRegistry<Payload>` 表达, 避免为每种按钮再造一个"命中向量 + 成员 Box"。

需要注意: **同一组件的多个命中表各自 `beginFrame()`**, 混用时要保证"清空 -> 渲染登记 ->
事件查询"的顺序 (查询必须发生在下一帧渲染前)。

### 2.4 声明式条目列表

[framework/ui_action_list.h](/agent/client/include/agentxx-client/io/tui/framework/ui_action_list.h) 把
"若干可选项 + 上下键 + Enter + 鼠标点击 + 选中高亮" 收敛为一张数据表:

```c++
struct UiActionItem {
    std::string           id;          // 命中标识 (唯一, 选中项按它保持)
    std::string           label;       // 主文本
    std::string           value;       // 右侧当前值 (可选)
    std::string           hint;        // 次级说明 (弱化色, 可选)
    bool                  enabled;     // 不可用时弱化且不可激活
    std::function<void()> onActivate;  // Enter / 点击命中时执行的动作
};
```

`UiActionList` 负责: 选中项维护 (`setItems` 后按 id 保持)、键盘导航 (Up/Down/Home/End,
跳过 disabled)、Enter 激活、鼠标点击命中后"先置选中再激活"、渲染时逐项登记命中、整行高亮;
需要多行版式时传 `rowBuilder` (设置弹窗的两行条目、会话弹窗的两行条目都是这样实现的)。

用法见 `SettingsOverlay` / `LogMenuOverlay` / `ModelSelectorOverlay` /
`SessionSelectorOverlay` 的实现: 新增一个设置项只需往条目表里加一行,
不必再改成员变量、渲染分支、键盘分支或鼠标分支。

### 2.5 模态 (弹窗)

- [ModalContainer](/agent/client/include/agentxx-client/io/tui/framework/modal_container.h) 持有
  `main` 与至多一个 `activeModal`:
  - `pushModal()/popModal()` 开关; 打开时渲染只渲染弹窗 (全屏蒙版色 + 居中), 主界面整棵树不参与该帧;
  - 事件只给弹窗: **未处理的事件也不再下发**给被遮挡的主界面
    (否则字符会落进被遮挡的输入框、滚轮会滚动被遮挡的消息列表);
  - 全局快捷键由外层 `CatchEvent` 在弹窗之前处理 (Ctrl+C 退出等), 不受影响。
- 弹窗内的按钮同样走命中登记表; 弹窗 `onClose` 由组件回调触发 (关闭会销毁弹窗对象,
  因此**不要在条目激活动作内直接关闭** —— 记录"待关闭"标志, 待条目列表事件处理返回后再关闭,
  见 `ModelSelectorOverlay::flushActivation` / `SessionSelectorOverlay::flushActivation`)。

### 2.6 插件 UI

- **注册入口只在 client 侧**: 面板/Info 段落/状态栏项/装饰/overlay 都由 client 侧插件
  经 `agentxx.client.ui` 表注册 (插件在 client 进程内, 描述不经网络); agent 侧插件要把
  数据交给同名 client 插件时走 `plugin_data` 转发, 自己不产生 UI 描述。唯一由 agent 侧
  产出、client 侧渲染的 UI 结构是**中断描述** (`interrupt_ui.h`)。
- 描述形态是 `agentxx.ui.item` schema 的 JSON (组件树, 见 [plugins.md](plugins.md) §9.1);
  client 侧由 [ui_components.h](/agent/client/include/agentxx-client/io/tui/ui_components.h)
  统一渲染为 Element, 并把元素内可点位置登记为可命中区域 (按钮/表格单元格/折叠标题/控件)。
- 命中与派发: 侧边栏内容经 `Scrollable::hitTestItem` 定位子项 + 局部坐标后判定区域
  (不用子项内的 `reflect`, 见 3.1); 命中后经 `ClientPluginManager::dispatchAction`
  投递回 io 线程, 二次校验插件存在/启用/实例代次后调用插件回调。
- 表单: 控件 (checkbox/select/buttons/number/text) 与提交行的状态由宿主维护, 提交经
  动作通道回传 `__submit` (参数 `{"values":{控件 id: 值}}`), 取消回传 `__cancel`,
  `commitOnPick` 的候选项点击即提交。插件不接触 UI 线程, 只收结果。
- 尺寸感知: 宿主布局后把面板/Info 段落的可用宽高记入快照, 值变化时投递
  `AGENTXX_CLIENT_EVT_UI_LAYOUT` 事件 (载荷 `{"regions":[{id,w,h}]}`), 并可经
  `get_client_state().regions` 查询; 插件据此按可用宽度重排内容。
- 相关 schema 与约束见 [plugins.md](plugins.md) §9.1~§9.4 与
  [ui_components.h](/agent/client/include/agentxx-client/io/tui/ui_components.h)。

---

## 3. 必须遵守的约束

### 3.1 命中区域与元素生命周期

- 命中表 `beginFrame()` 会释放上一帧的 `Box`。**若持有旧 Element 的容器跨帧缓存该元素**,
  其 `reflect` 节点仍引用已释放的 `Box` (悬空引用), 再次布局即非法访问。
  因此:
  - 登记了命中的元素**不要**放进跨帧缓存 (消息列表把"含 decor 按钮/中断控件"的消息标记为
    `cacheable = false` 正是为此);
  - 或者像 banner 的 [重试] 按钮那样**不每帧清空** (`MessageListComponent::bannerHits_`):
    缓存命中时命中项与元素一起跨帧存活 (Box 由同一 Element 的 `reflect` 每帧更新),
    仅在"元素重建"或"元素离开列表"时清空;
  - 清空登记表后, 旧元素必须在本帧**重新渲染前**被替换 (本项目各组件都是
    "OnRender 帧首清空 -> 同帧重新构建元素", 次序天然满足)。
- 组件"消失"的判定以**是否渲染**为准, 不要再用 `Box{}` 之类的"清零"表达
  (它等于屏幕左上角, 会造成 (0,0) 处误触)。
- **滚动容器内的可点击子项用 `Scrollable::hitTestItem` (或 `visibleBoxes()`), 不要用子项
  元素内的 `reflect`**: [Scrollable](/agent/client/include/agentxx-client/io/tui/scrollable.h) /
  [LazyScrollable](/agent/client/include/agentxx-client/io/tui/lazy_scrollable.h)
  测量子项高度时会以"测量用临时大框" (局部坐标: x = 0..内容宽, y = 0..很大) 调用 `SetBox`,
  之后只有视口内的子项会被重新定位到真实屏幕坐标, 视口外 (上方/下方) 的子项会残留该大框。
  大框的局部 `x` 与屏幕坐标的左侧区域重叠, 于是按 `reflect` 框命中的点击会命中到
  **看不见的子项** (通常是列表中靠前的那条), 且横向点击位置不同命中结果还不同。
  正确做法 (侧边栏面板/Info 段落/overlay 已按此实现):
  - 渲染子项时把可点位置写入 `ScrollItem::hits` (`UiHitRegion`: 相对子项左上角的局部矩形
    + 类型 + 标识 + 参数; `w <= 0` 表示延伸到子项右边界);
  - 事件里用 `hitTestItem(x, y, itemIndex, localX, localY)` 定位到**可见**子项并取局部坐标
    (子项被视口上边缘裁剪时仍按子项顶边换算), 再与 `hits` 比对;
  - `Scrollable` 会在子项离开视口时清空它的布局框, 避免残留命中区。
  [ContextOverlay](/agent/client/src/io/tui/components/overlays.cpp) 的 `headerMessageAt`
  与 `MessageListComponent` 的可见区域命中登记是同一做法的其它形态。
- **测量与渲染必须同源**: 组件描述的行数由 `ui_components.cpp` 的 `measureItem` 得出
  (内部渲染一次统计行数), 不要另写一套"估算行数"的判定 —— 两套判定漂移会让滚动位置与
  高度计算错位。回归保护见 `agentxx_test tui_ui_items` (估算行数 == 元素真实布局高度)。

### 3.2 事件消费

- 组件只在**真正处理**了事件时返回 `true`; 鼠标事件未命中任何登记项时应返回 `false`,
  让其它组件或全局处理继续 (否则会吞掉别人/全局的点击)。
- 弹窗组件例外: 作为遮挡层, 未处理的事件也应 `return true` 吞掉 (见 2.5)。

### 3.3 渲染性能

- 只渲染需要渲染的内容: 主界面在弹窗打开时不渲染 (ModalContainer 已处理);
  侧边栏 tab 未激活时不渲染其内容; 日志 tab 未激活时日志更新不触发重绘。
- 大列表使用 `LazyScrollable`, 让不可见项零成本, 并把"渲染结果的内存估算"用于缓存预算。
- 每帧只查一次终端尺寸 (`ctx_.terminalSize()`), 不直接调用 `ftxui::Terminal::Size()`
  (Linux 上是 `ioctl` 系统调用), 同时保证同帧内布局口径一致。

### 3.4 文案与样式

- 界面文案一律经 `tr()/trf()` 取; 技术字段 (role/args 等) 不翻译。
- 弹窗一律用 `tuiSurfacePopup/tuiSurfaceFrame/tuiSurfaceToast` 组装; 不要自行拼边框。
- `bgcolor(ftxui::Color::Default)` 会把单元格背景重置为终端默认色 (抹掉父级背景),
  需要"不着色"时用 `UiActionStyle` 的默认值语义 (框架内部已跳过 Default)。

---

## 4. 本次整理修复的问题 (回归测试见 `agentxx_test tui_widget`)

| 问题 | 位置 | 处理 |
|------|------|------|
| "未显示的按钮仍能点中" | 各弹窗/状态栏/输入栏的成员 `Box` | 改为命中登记表: 帧首清空 + 渲染时登记 |
| `Box{}` 被当成"无命中区域" | 输入栏"待发队列"、banner 重试按钮等 | 空区域统一用 `kNoBox`/`IsEmpty()`, 并加回归测试 |
| 每个按钮一个成员变量 + `OnEvent` 里逐项 `Contain` | 设置/日志菜单/模型/会话/待发队列/文件选择弹窗 | 收敛到 `UiActionList` (条目表) 与 `UiHitMap` |
| 条目索引魔法数字 (`selectedIndex_ == 2` 之类) 与 4 处同步修改 | 设置弹窗等 | 条目即数据 (`id/label/value/onActivate`), 新增条目只改一处 |
| 模态打开时事件漏到被遮挡的主界面 (字符落进输入框、滚轮滚动消息列表) | `ModalContainer` + `FilePickerOverlay` | 模态阻塞事件; 弹窗自身也吞掉未处理事件 |
| 模态打开时仍每帧渲染主界面整棵树 | `ModalContainer` | 弹窗打开时不渲染主界面 |
| 每帧多次 `Terminal::Size()` (ioctl), 帧中途 resize 可能造成组件间尺寸不一致 | 各组件 | `TUICtx::refreshFrameSize()` 帧首刷新 + `terminalSize()` |
| 侧边栏 tab 列表维护"可见下标 -> tab 下标"映射表 | `SidebarComponent` | 命中载荷直接携带条目归属 |
| 状态栏/输入栏/消息列表的按钮点击散落在全局事件处理里 | `agent_tui.cpp` | 各组件自行处理自身区域内点击 (回调在装配时注入) |
| 上下文弹窗点击消息, 展开/折叠的是"别的消息" (消息多、滚动后尤其明显) | `ContextOverlay` 折叠头命中框 (子项元素内的 `reflect`: 视口外子项残留测量大框) | 改为按 `Scrollable::visibleBoxes()` + 子项->消息下标映射命中 (回归测试见 `agentxx_test tui_context_overlay`) |
