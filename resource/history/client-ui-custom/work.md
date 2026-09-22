# 客户端 UI 能力扩展 —— 实施记录

- 方案文档: [plan.md](plan.md)
- 基于 commit: `ecc97b94`（设计文档提交）
- 记录规则: 每完成一个阶段更新本文件并 git 提交

---

## 当前状态

| 阶段 | 内容 | 状态 |
|---|---|---|
| P1.1 | lib 组件描述层（schema 解析/校验/序列化/纯文本降级/文本宽度） | ✅ 已完成 |
| P1.2 | 客户端唯一渲染实现 `ui_components`（全部 kind） | ✅ 已完成 |
| P1.3 | 子区域命中（`UiHitRegion` / `addRegions` / `Scrollable::hitTestItem`） | ✅ 已完成 |
| P1.4 | 接入点收敛（面板 / Info / 装饰 / overlay / 中断内容块） | ✅ 已完成 |
| P1.5 | 中断描述扩展（`raw` 透传 + `custom` 派发 + 扩展组件） | ✅ 已完成 |
| P1.6 | SDK 构建器 `agentxx::ui::Items` + `ClientPluginBase` 便捷方法 | ✅ 已完成 |
| P1.7 | 测试（`ui_items` 199 项 + `tui_ui_items` 149 项） | ✅ 已完成 |
| P1.8 | 文档（plugins.md / tui.md / index.md / AGENTS.md） | ✅ 已完成 |
| P2.2 | 表单交互（控件编辑 + `__submit`/`__cancel`/`commitOnPick` 经动作通道回传） | ✅ 已完成 |
| P2.4 | overlay 尺寸与外观选项（`size`/frac/`footer`/`scroll`/`stack`） | ✅ 已完成 |
| P2.3 | 尺寸感知（布局快照 + `EVT_UI_LAYOUT` + `regionSize()`） | ✅ 已完成 |
| P2.5 | 状态栏 segments / sparkline / meter（单行） | ✅ 已完成 |
| P2.1 | 中断控件布局迁移到共享实现（`UiFormState` + 共享渲染/交互/校验） | ✅ 已完成 |
| P3 | `agentxx.client.timer` / `agentxx.client.keybind` 新表 | ⬜ 待开始 |

---

## 阶段 1：组件描述层与渲染收敛（已完成）

### 新增文件

- `agent/lib/include/agentxx/ui/text_width.h`
- `agent/lib/src/ui/text_width.cpp`
  - 终端显示列宽（宽字符 2 列 / 组合字符 0 列 / emoji 2 列）、按列宽截断、右侧补齐、
    按列宽取前缀；不依赖任何 UI 库，宿主与插件都可直接用
- `agent/lib/include/agentxx/ui/item.h`
- `agent/lib/src/ui/item.cpp`
  - `agentxx.ui.item` schema 的规范化模型 + 解析（容错、上限、未知 kind 标记 `known=false`）
    + 序列化（往返保留）+ 纯文本降级 `plainText`
  - 支持的 kind：`text` / `markdown` / `diff` / `separator` / `gap` / `badge` / `diagram` /
    `button`(别名 `action`) / `progress`(归一化为 `meter`) / `meter` / `sparkline` /
    `kv` / `table` / `tree` / `row` / `box` / `collapse` / `control` / `submit` /
    `custom` / `canvas`(仅解析与降级)
- `agent/lib/include/agentxx/ui/build.h`
  - 链式组件树构建器 `agentxx::ui::Items`（header-only，产出插件接口能直接吃的 JSON）
- `agent/client/include/agentxx-client/io/tui/ui_components.h`
- `agent/client/src/io/tui/ui_components.cpp`
  - 终端侧唯一渲染实现（面板 / Info 段落 / 工具消息装饰 / 通用 overlay / 中断内容块共用）
  - 渲染产出"行模型"：元素 + 行数 + 元素内可命中区域；`measureItem` 复用同一条渲染路径，
    估算与渲染不会有第二套判定

### 删除 / 更名

- `ui_items_render.h` / `ui_items_render.cpp` → 由 `ui_components.{h,cpp}` 取代

### 主要改动

- `framework/ui_hit.h`：新增 `UiHitRegion`（局部坐标矩形 + 类型 + 标识 + 参数 + 子序号 +
  归属信息）、`UiHitRegistry::addRegions` / `findRegion` / `findRegionClick`、
  `matchUiHitRegion`
- `scrollable.{h,cpp}`：`ScrollItem::hits` 承载子项内可命中区域；新增
  `hitTestItem(x,y,index,localX,localY)`（含被视口裁剪时仍按子项顶边换算局部坐标）；
  离开视口的子项显式清空布局框，消除"看不见却点得中"的残留命中区
- `agent_tui.{h,cpp}`：面板渲染改用共享组件层；点击改经 `handleSidebarRegionClick`
  （滚动容器定位子项 + 区域判定）；新增折叠展开状态表；面板 markdown 生命周期按帧轮换保存
- `tui_sidebar_content.cpp`：Info 段落渲染改用共享组件层
- `components/message_list.{h,cpp}`：装饰渲染改用共享组件层；`DecorHitBox` 增加 `regions`，
  点击按局部坐标定位到具体区域（表格单元格等）
- `components/overlays.cpp`：`CustomOverlay` 渲染与命中改用共享组件层与
  `Scrollable::hitTestItem`；面性弹窗的分隔线风格经 `UiRenderCtx::separatorStyle` 保留
  （弹窗用整行浅色区块，不画横线）
- `components/interrupt_view.{h,cpp}`：内容块经共享组件层渲染，未知 kind 走
  `InterruptUiBlock::raw` 解析（中断描述可直接使用表格/树/横排/分组/趋势图），
  `custom` 块派发到共享组件（`component` + `props`），无内容时降级 `fallback`
- `middlewares/interrupt_ui.{h,cpp}`：`InterruptUiBlock` 新增 `raw`（解析时保留原始 JSON，
  序列化以它为底，扩展字段往返不丢）
- `tui_i18n.cpp`：新增 `ui.submit` / `ui.cancel` / `ui.none` 三条共用文案

### 验证

- Windows Debug 编译通过（lib / client / test 三个目标）
- `agentxx_test.exe` 全量：**22137 项断言全部通过**（含全部 TUI 与中断用例）

---

## 阶段 2：组件层测试（已完成）

### 新增测试模块

- `test/core/test_ui_items.cpp`（lib，193 项断言）：schema 解析与归一化、往返序列化、
  解析上限（深度/元素数/文本长度）、纯文本降级、显示列宽工具、构建器产出形状
- `test/client/test_tui_ui_items.cpp`（client，98 项断言）：各 kind 的屏幕渲染、
  估算行数与真实布局高度一致、元素内可命中区域坐标与标识、
  `Scrollable::hitTestItem` 滚动前后命中映射、面性弹窗分隔线风格

### 测试中发现并修复的问题

1. `renderKeyValue` 在 `std::move(lines)` 之后读 `lines.size()` → 移动后行数变 0
   （kv 只占 1 行，滚动定位会错）；已在 move 前取行数
2. `submit` 行在无表单状态时也登记提交区域 → 与控件口径不一致（没有状态就没有值可提交）；
   改为只有 `ctx.form != nullptr` 时才登记
3. `Item::interactive()` 未递归树节点子级 → 子节点带动作时漏判
4. 构建器/序列化里 `Json{x}` 会走 `Json(std::initializer_list<Json>)`，构造出
   **单元素数组**而不是标量；全部改为 `Json(x)` 或逐字段赋值
   （`Json::object({{"k", Json{k}}})` 这种写法同样中招）

### 测试辅助经验

- 断言屏幕内容时不要用 `Screen::ToString()`：它会把颜色转义序列一起输出，
  子串会被转义码打断；应逐格读 `PixelAt(x,y).character` 拼文本
  （参考 `test/client/test_tui_ui_items.cpp` 的 `renderToText`）
- 测量一致性断言：`measureItem` 的行数须等于元素真实布局高度
  （`layoutAndMeasure`），这是"测量与渲染同源"的回归保护

---

## 阶段 3：SDK 便捷方法（已完成）

`agent/lib/include/agentxx/plugin/api/plugin_kit.h` 的 `ClientPluginBase` 新增：

- `setPanelItems(panel, ui)` / `setPanelJson(panel, json)`：面板内容直接提交组件树
- `setInfoSectionItems(section, ui)` / `setStatusText(item, text)` / `setStatusJson(item, json)`
- `showItemsOverlay(title, ui, extraJson)` / `showOverlay(type, title, payload, extraJson)`
- `hostSupports(capability)` / `clientStateJson()`：能力协商查询（解析客户端状态里的 `interfaces`）

---

## 阶段 4：表单交互（已完成）

控件（checkbox / select / buttons / number / text）与提交行在**面板、Info 段落、通用 overlay**
里都可交互，状态由宿主维护、结果经既有动作通道回传：

- 共享交互实现：`ui_components.h` 的 `handleFormControlHit` / `handleFormSubmitHit` /
  `handleFormKeyInput` / `validateForm` / `formValues` / `initFormState`
- 归属状态：`TUIClientAgentIO::pluginForms_`（键 = 面板 id / 段落 id）；
  `CustomOverlay` 内另有独立表单状态（键 = `__overlay`）
- 结果契约：提交 → 动作 id `__submit`、参数 `{"values":{控件 id: 值}}`；
  取消 → `__cancel`；`commitOnPick` 的候选项点击即提交（`actionId = 控件 id`）
- 键盘：点击控件即聚焦（`formFocusedOwner_`），字符键/退格/Delete 编辑，Tab 在控件间移动，
  Esc 释放焦点，回车提交；数值框过滤非数字字符，提交前校验范围（失败写提示不提交）
- 面板/段落内容更新时按描述补齐并清理控件状态（描述里已删除的控件不残留）

### 测试

- `tui_ui_items` 新增 51 项断言：初始化、点击命中（勾选/选中/步进/越界/未知控件）、
  键盘输入（替换缺省值/退格/Tab/数值过滤/Esc）、校验与取值、容器内控件

### 踩坑

- `controlAliasOf(item.kind)` 返回的是 `string_view`，指向 `item.kind` 的缓冲区；
  先取 view 再改写 `item.kind` 会让它悬空（短字符串共用同一缓冲区）→ 必须先拷贝成 `std::string`
- `formValues` 递归合并时不要把内层 `{"values":{...}}` 整包并进外层（会多一层嵌套），
  应把控件值收集到同一层对象

1. P1.6 SDK 便捷方法（`plugin_kit.h`：`setPanelItems` / `showOverlay` / `regionSize` 等）
3. P2：
   - `UiFormState` 接入面板/overlay（控件值编辑、`__submit`/`__cancel`/`commitOnPick` 经
     `dispatchAction` 回传），并把 `InterruptView` 的控件布局迁移到共享实现
   - 尺寸感知：布局快照 + `AGENTXX_CLIENT_EVT_UI_LAYOUT` 事件 + `regionSize()`
   - overlay 尺寸选项（`size` / `width_frac` / `height_frac` / `scroll` / `footer` /
     `stack`）与"已有模态时的替换策略"修正
   - 状态栏 segments / sparkline / meter（单行）
4. P3：`agentxx.client.timer` v1、`agentxx.client.keybind` v1 及对应 SDK/测试
5. 文档：`plugins.md` §9 扩展 + 新增组件 schema 小节；`tui.md` §2.2/§2.6/§3；
   `index.md` 客户端 UI 节；`AGENTS.md` 记忆行

---

## 阶段 5：通用 overlay 尺寸与外观选项（已完成）

`open_overlay` 的 `extra_json` 从"只消费 TEXT 的 markdown"扩展为完整选项
（数据层，零 ABI 变更；老宿主忽略未知键）：

- `size`: auto / compact / normal（缺省）/ large / full
- `width_frac` / `height_frac`: 显式比例（与 `size` 同时给出时比例优先）
- `footer`: 是否显示底栏提示；`scroll: false` 视为内容不需滚动（隐藏提示）
- `stack`: 已有 overlay/核心弹窗时的策略 —— 缺省**替换**（last-wins，修正此前
  "已有模态直接 return" 与接口注释不一致的漂移），`stack: true` 保留现有并丢弃本次

实现：`OverlayOptions`（`components/overlays.h` 的 `fromJson` / `resolveFractions`）+
`overlayFrame` 接受选项 + `createUniversalOverlay` 统一设置；测试见 `tui_surface`。

---

## 阶段 6：展示区域尺寸感知（已完成）

- 新事件 `AGENTXX_CLIENT_EVT_UI_LAYOUT`（枚举追加值，老宿主对新值订阅失败即降级）：
  载荷 `{"regions":[{"id","w","h"}]}`，在尺寸变化时向订阅者投递（io 线程）
- 宿主快照：`ClientPluginManager::reportRegionSize(id, w, h)`（UI 线程，值未变化直接返回）
  + `regionSizes()`；同时进入 `get_client_state().regions`，SDK 提供
  `ClientPluginBase::regionSize(regionId)` 查询
- UI 侧上报点：面板渲染回调与 Info 段落渲染（内容行数 + 可用宽度）
- 能力名 `agentxx.client.layout`（TUI 适配器声明）
- 测试：`client_plugins` 覆盖上报/去重/覆盖/非法值忽略与状态快照

---

## 阶段 7：状态栏富展示片段（已完成）

状态栏项 JSON 除 `text`/`tooltip` 外支持单行富片段（`segments` / `sparkline` / `meter`），
由共享组件层渲染为一行；只给 `text` 时行为与之前完全一致（含 24 字截断）。实现要点：

- `ClientStatusItem` 增加 `rich`（注册与更新时解析，随 UI 注册表 COW 快照走）
- `status_bar.cpp` 的 `statusRichElement()` 把片段组装成一个 `row` 组件树交
  `ui_components` 渲染（sparkline 高度强制 1；单侧宽度预算 = 终端宽度 / 3）
- 状态栏的注册表来源改为"本帧快照优先，其次管理器快照"（可测性 + 与其它组件一致）
- 测试：`tui_widget` 覆盖富片段渲染成功与降级文本不出现、纯文本项行为不变

---

## 阶段 8：中断表单迁移到共享实现（已完成）

P1.4/P2.1 的收尾：`InterruptView` 不再自己渲染控件，全部走共享组件层。

### 共享层扩展（`ui_components.cpp`）

- 控件外观统一到一套样式（中断与插件表单同款）：勾选项 `[ ✓ ] / [   ]`（勾选时强调色加粗）、
  单选列表选中项 `▸ ` + 反色底（未选中同宽占位）、输入框为背景填充的字段（聚焦时加粗下划线）、
  数值控件 `[ - ] value [ + ]`（步进按钮用按钮底色）
- `handleFormKeyInput` 补齐非输入类控件的键盘语义（此前只有字符/退格/Tab/Esc）：
  `buttons` 左右切换选中项、`select` 上下切换、`number` 上下步进（受 min/max 约束）、
  `checkbox` 空格翻转；输入框的左右方向键被消费（单行无光标定位，不落到滚动）
- 输入框首次输入**替换缺省值**（原先共享实现是"在缺省值后追加"，与中断不一致，已统一为替换）
- `validateForm` 增加"候选项缺失"校验（`buttons`/`select` 无 options → 写提示并拒绝提交）；
  `formValues` 对未知控件形态不再产出值，数值控件按 `integer` 写整数（保持中断结果契约）
- 新增 i18n 文案 `ui.noOptions`

### `InterruptView` 改动

- 状态结构改为共享层类型：`using ControlState = UiFormControlState`、`using FormState = UiFormState`
  （`FormState::control()` 访问器删除，改用 `find()`）
- 删除 `layoutControl` / `layoutSubmit` / `initControlState` / `step` / `renderValueButton`
  及配套的数值/选项私有 helper（净减 ~570 行）
- 新增 `formItems()`（描述 → 控件/提交行组件项，标签与说明的 i18n 键在此解析；提交行缺省文案
  沿用中断词表 `确认`/`✕`）、`plainItems()`、`firstControlId()`、`registerBlockHits()`
- `layoutForm`：控件块与提交行交给 `renderItem`（`rc.form` 传表单状态），内容块/扩展组件不变；
  命中区域由行模型的 `UiHitRegion` 归因到"块下标 + 控件 id + 子序号"（只登记控件与提交行区域，
  扩展组件内的普通动作区域在中断里没有去处，不参与命中）
- `handleClick`：命中判定改为"行元素框 + 行内区域"两级；控件语义交给
  `handleFormControlHit`（含 commitOnPick → 提交），提交行按 `__submit`/`__cancel` 映射确认/取消
- `handleKey`：回车提交、Esc 释放激活状态；其余按键交给 `handleFormKeyInput`
  （焦点缺失时回落到首个控件；`Tab` 现在也能在控件间移动焦点）
- `confirm`：校验与取值改为 `validateForm` + `formValues`，结果展示文本仍按块顺序拼"标签: 值"

### 测试

- `test_tui_interrupt.cpp`：点击辅助改为按"行元素框 + 区域内点"点击（`clickHit`），
  控件状态读取改用 `find()`；数值非法输入用例改为输入 `1.2.3`（数值框现在过滤字母）；
  未知控件诊断行断言改为共享层的 `[control: xxx]`
- `test_tui_ui_items.cpp`：单选列表断言改为 `▸ B` / `  A`；输入框首次输入断言改为替换语义；
  新增非输入类控件键盘用例（buttons 左右/select 上下/checkbox 空格）与数值方向键步进
- Windows Debug 全量测试：**22245 项断言全部通过**（含 `tui_interrupt` 218 项、`tui_ui_items` 169 项）

### 文档

- `plugins.md` §9.2：补齐键盘语义（方向键/空格/首次输入替换）与"中断表单复用同一实现"的说明
- `tui.md` §2.2/§2.6：说明中断全部块都经 `ui_components` 渲染、命中为两级判定
- `interrupt_ui.h`：`custom` 块的"未实现"TODO 更新为已派发共享组件的说明

### 现在只在一处实现的清单

| 能力 | 唯一实现 |
|---|---|
| 组件解析/校验/上限/纯文本降级 | `agentxx/ui/item.cpp` |
| 组件渲染 + 测量 | `ui_components.cpp` |
| 控件状态/点击/键盘/校验/取值 | `ui_components.cpp` 表单函数 |
| 命中区域 | `UiHitRegion`（行模型）+ `Scrollable::hitTestItem` |
| 结果回传 | 插件表单 = 动作通道；中断 = 结果通道（同一份控件语义） |

---

## 待完成任务（下一步）

1. P3：`agentxx.client.timer` v1（一次性/周期定时器 + 可见性/动画等级门控）、
   `agentxx.client.keybind` v1（全局快捷键注册与派发）及对应 SDK/测试
2. 基准：`benchmark` 增加"含面板（表格 + sparkline + 表单）的 TUI"帧耗时与内存采样
3. `canvas` 完全自绘（本版只做类型预留与降级；渲染/命中/输入留待后续单独设计）

---

## 注意事项（实施中记录）

### 兼容性

- `progress` kind 在解析时归一化为 `meter`（`value` 由 0..1 比例换算为 0..100 绝对值），
  渲染只有一个实现
- 按钮 action id 三种历史写法都要认：`action`（新）、`action_id`（kind=button 旧写法）、
  `id`（kind=action 旧写法）
- `text` + `button` 相邻项仍按历史行为合并为一行（面板/Info/overlay/装饰统一），
  合并行内的按钮区域坐标按文本显示宽度右移
- 面性弹窗（overlay）的分隔线不能画 `─`（有测试断言"弹窗内无框线/分隔线字符"），
  经 `UiRenderCtx::separatorStyle` 区分

### 踩坑

- `ftxui::color()` 会被同名的局部变量/形参遮蔽（`std::string_view color`），
  组件渲染函数内的颜色变量统一命名 `colorName` / `textColor` / `fillColor`
- 滚动容器内**不能**用子项元素内的 `reflect` 做命中判定：容器测量子项时会用
  "局部坐标大框"布局，视口外子项会残留该框造成幽灵命中（本次已改为
  `hitTestItem` + 区域映射，并在子项离开视口时清空其布局框）
- `bgcolor` 装饰器按"元素被分配的框"着色：要整行铺背景，装饰器必须套在整个行元素上
  （放进 `hbox` 内的子元素只会拿到自身宽度）
- markdown 的 `DomBuilder` 必须与 Element 同生命周期；面板/Info 每帧重建，故按帧保留两代

### 约定

- 新增组件只需改三处：`agentxx/ui/item.h`（字段与解析）、`ui_components.cpp`（渲染 + 测量）、
  对应测试；插件接口表不动
- 测量一律走 `measureItem`（内部渲染一次统计行数），禁止另写一套估算
- 控件（`control`/`submit`）的渲染、点击、键盘、校验、取值只有 `ui_components.cpp` 一份实现：
  新接入点一律传 `UiRenderCtx::form`（`UiFormState`），不要再写控件分支
- 中断/插件表单的差异只允许出现在"结果去处"：中断 → 结果通道（`InterruptSend`），
  插件 → 动作通道（`__submit`/`__cancel`/`commitOnPick` 经 `dispatchAction`）

### 中断迁移（阶段 8）踩过的坑

- 命中必须"行元素框 + 行内区域"两级判定：共享层只给出"元素 + 元素内区域"，
  中断需要把区域归因到（块下标、控件 id、子序号），且区域矩形要一起存下来
  （整行区域 `w <= 0` 时取区域左端点击，`w > 0` 取区域中点）
- 结果 JSON 的类型要按描述保持：整数控件必须写整数（`{"count": 42}` 而非 `42.0`），
  否则中断结果消费方（`interruptValueInteger` 等）与历史契约不一致
- 未知控件形态（`control: "future_widget"`）不参与结果：`formValues` 里别用 `else` 兜底，
  只对已知形态（checkbox/buttons/select/number/text）写值
- 中断的提交行缺省文案是 `确认` / `✕`（`interrupt.confirm` / `interrupt.cancel`），
  与插件表单的 `提交` / `取消`（`ui.submit` / `ui.cancel`）不同 —— 转换块时要补默认值
- 数值框会过滤非数字字符，所以"输入非法值再校验"的用例要改用可键入但无法解析的内容
  （如 `1.2.3`），而不是 `abc`
