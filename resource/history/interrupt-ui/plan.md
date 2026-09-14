# 中断渲染通用化 —— 阶段 3 实施计划

> 状态: 阶段 1/2 与"历史兼容清理"已完成并全量测试通过；阶段 3 待评估排期。
> 相关文档: [design/index.md](../../../docs/zh-cn/design/index.md)(客户端 UI / PermissionMiddleware / HIL)、
> [plugins.md](../../../docs/zh-cn/design/plugins.md)、[ffi.md](../../../docs/zh-cn/design/ffi.md)。

## 0. 前置结论(已完成部分)

### 0.1 阶段 1: 抽出通用视图(纯整理)

`message_list.cpp` 中的中断渲染/估算/交互/状态抽到独立模块，消息列表只保留转发调用。

### 0.2 阶段 2: 方案 A(声明式中断描述)

中断 = **声明式 UI 描述数据** + 客户端**通用表单渲染器**，客户端不再包含任何具体询问类型
(含 permission) 的分支：

- `agent/lib/include/agentxx/middlewares/interrupt_ui.h`(schema)
  - `InterruptUi{version, header{progress,label,segments[]}, items[], values[], options[]}`
  - 项类型: `text` / `gap` / `toggle` / `input` / `submit` / `separator` / `diff`
  - 输入形态 `view`: `buttons`(值按钮, 点击即确认) / `number`(减-输入框-加) / `text` / `list`(枚举列表)
  - 描述字段留空 = 取消息字段(模板语义)，故一份描述可服务多输入项的中断请求
- `InterruptUi::defaultUi()`(通用默认描述) / `InterruptUi::permissionUi(tool,category,target)`(权限卡片)
  / `makeInterruptResult(values, options)`(结果组装)
- `agent/client/.../components/interrupt_view.{h,cpp}`: 通用渲染/估算/命中/交互/结果组装
  (渲染与估算同一套项判定)；`text_layout.{h,cpp}` 抽出共用文本布局辅助
- 结果协议: `{"values":[...], "options":{...}}`
- "记住本次选择": 权限处理器按 `options.remember` 在 **agent 侧** 经 `EventSetPermissionRule`
  注册路径规则(客户端不再发 `WireSetPermission`，不参与权限语义)

### 0.3 历史兼容清理(本轮)

- 描述**必填**: `InterruptHandleArg::toJson` 在生产方未声明时下发通用默认描述；
  客户端无"无描述"渲染回退 —— 缺失即输出诊断行且不可交互
- 结果**恒对象形态**: 消费端(HIL / 权限 / FFI)不再解析"纯值数组"；契约外载荷按
  未应答(HIL) / 拒绝(权限)处理并告警；`agentxx_ffi_interrupt_respond` 对非对象载荷返回
  `AGENTXX_FFI_ERR_INVALID`
- 旧宿主兼容字段清理: planning 插件按钮不再双发 `mermaid`(改由 `action_id` 派发状态图)

---

## 1. 阶段 3 任务清单

优先级: P0 = 收益直接且风险低；P1 = 机制开放；P2 = 能力扩展(需先定协议版本)。

### 3.1 [P0] stdio CLI 按描述渲染

- **现状**: `StdIOClientAgentIO::handleInterrupt` 只按 `inputs[]` 逐行问答(通用文本)，
  读不到描述语义 —— 无分段头、无勾选项、无"记住选择"，与 TUI 行为不对齐。
- **目标**: 描述 → 文本化渲染(头行分段 / text / gap / toggle 行 / input 行 / submit 行)，
  勾选项同样经结果 `options` 回传，使 CLI 也具备"记住本次选择"等能力。
- **要点**:
  - 文本渲染器可放 `agent/client/src/io/stdio/` 或与 TUI 共享的"描述 → 文本"helper；
  - 输入解析: `buttons`(输入序号/yes/no)、`number`(数值+步进语法)、`list`(序号选择)、`text`；
  - 勾选项: 交互式勾选(`y/n`)或 `--remember` 之类命令行附加参数(需定交互形态)；
  - 描述缺失(契约违规)时的诊断输出。
- **风险**: 交互形态设计(单行终端下多控件如何呈现)需先给出样例再实现。
- **规模**: 中(~300 行 + 测试)。

### 3.2 [P0] FFI 宿主能力补齐与文档

- **现状**: `EVT_INTERRUPT_REQ` 的 `argJson` 已含必填 `ui`(已更新 ffi.md)，但文档未给出
  渲染指引；`agentxx_ffi_interrupt_respond` 已要求对象形态(阶段 0.3 完成)。
- **目标**: 宿主 GUI 可"零语义"通用渲染中断；宿主也能使用勾选项。
- **要点**:
  - `ffi.md`(zh/en) 增加"中断 UI 描述渲染指引"小节: 项类型表 + 结果映射 + 示例 JSON；
  - SDK/示例(如 `agent/lib/src/ffi/` 或文档中的伪代码)演示 `toggle` → `options` 回传；
  - 评估是否需要"描述版本协商"(见 3.6)。
- **风险**: 文档/ABI 承诺变更需标注版本号。
- **规模**: 小~中(文档为主，~150 行示例)。

### 3.3 [P1] 中断描述的多来源

- **现状**: 描述只由核心生成(权限卡片 / 默认模板)。
- **目标**: 允许插件声明中断 UI。
  - (a) **agent 侧插件**: `requestInterrupt` 时携带 `ui`(如 rag_search 确认、自定义审批、
    资源写操作确认)；
  - (b) 可选 **client 侧插件**: 按 `node` 注册渲染器(复用 `InterruptView` 与
    `plugin_ui_items` 共享 helper)，覆盖宿主自定义交互。
- **要点**:
  - (a) 需在插件 C ABI 的"中断参数"路径上开放 `ui` 字段(现由核心 `InterruptHandleArg`
    承载；插件经宿主接口构造中断请求时应可传入)；
  - (b) 需新的 client 插件接口(渲染器注册 + 命中派发)，注意多实例契约与代次校验。
- **风险**: 插件 API 必须保持纯 C ABI；描述 schema 需冻结(与 3.5 协同)。
- **规模**: 中(~200 行 + 插件示例 + 文档)。

### 3.4 [P1] options 的通用消费机制

- **现状**: 只有权限处理器消费 `options.remember`。
- **目标**: "描述声明 options → agent 侧注册消费回调"，让任意询问都能声明持久化选项：
  - 例: 以后不再提示该工具、默认模型偏好、通知开关、跳过某类确认。
- **要点**:
  - 描述中 `options` 项可带类型/默认值/作用域(会话级 / 全局)；
  - agent 侧注册"选项 id → 处理函数"(写配置 / 写会话偏好 / 写路径规则)；
  - 与 `InterruptHandleArg.resultId`、subagent 结果映射保持正交。
- **风险**: 需要"谁能声明选项"的边界(核心 + 插件?)，避免任意插件写全局配置。
- **规模**: 中。

### 3.5 [P2] 描述能力扩展(建议与协议版本一起做)

1. **一条消息内的多输入项(表单化)**: 现为"每个输入项一条消息"；改为单条消息内多控件，
   结果 `values` 顺序契约变化 —— 建议与 3.4 一起设计为 `version 2`。
2. **校验规则声明**: int/double 的 min/max、string 正则、必填等由服务端声明，
   客户端按声明校验(现为客户端内置校验)，错误提示文案可声明。
3. **`diagram` / `diff` 在中断中的落地**: 计划审批(roadmap 预览)、改动确认(带 diff)。
4. **富文本 `text` 项**: 支持 markdown。
5. **条件显示 / 禁用项**: 项级 `when`(依赖其它控件值)。
- **风险**: 2 会改变校验归属；1/5 会改变状态模型(每项独立状态)；需先冻结 schema。
- **规模**: 大(可拆分，按需求逐项做)。

### 3.6 [P1] 协议版本与能力协商

- **现状**: `InterruptUi.version = 1`；无协商。
- **目标**: 明确"描述版本"演进规则：
  - 客户端渲染各字段时**忽略未知项类型/未知字段**(已实现)；
  - 需要"新增能力检测"时(如 3.5 的表单化)，在 `Hello/HelloAck` 中交换支持的最高版本
    (与现有 `WireHelloAck.PluginInfo`/`client_interfaces` 同一思路)。
- **要点**: 服务端按客户端版本降级生成描述(描述生成端做兼容分支)，客户端不做旧行为分支。
- **规模**: 小。

### 3.7 [P2] TUI 交互增强

- **Tab/Shift+Tab 在项间移动焦点**(现为单击激活单焦点)。
- **枚举超长**: 列表滚动窗口(当前全量渲染，条目很多时占满视口)。
- **多中断并行**: 多个未决中断同时展示时的激活键管理(现仅一个 `activeMsg`)。
- **终态回显**: 勾选项在 Confirmed/Cancelled 状态行回显(现只显示结果值)。
- **输入体验**: 文本框光标移动/粘贴、数值范围提示。
- **规模**: 中。

### 3.8 [P0] 文档与收尾

- `plugins.md`: 记录"插件 items 词汇与中断描述共用同一渲染 helper/配色表"的边界与
  两套 schema 的差异(input/toggle/submit 仅中断侧，button/action_id 仅插件侧)。
- 权限卡片 badge 文案 i18n(现为字面 `! [Permission] `)；`interrupt.noDescriptor` 等键补齐。
- 更新 `TODOS.md`(移除已完成条目)。
- **规模**: 小。

---

## 2. 建议实施顺序

```
3.8(文档/收尾, 随时)
  → 3.1(stdio 对齐, 收益直接)
  → 3.2(FFI 宿主能力+文档)
  → 3.6(版本协商)
  → 3.4(options 通用消费)
  → 3.3(插件描述来源)
  → 3.5 / 3.7(能力扩展, 按需求排期; 3.5 需先定 version 2 协议)
```

## 3. 待决策点(实施前需确认)

| # | 决策 | 影响 |
|---|---|---|
| D1 | 是否把"一条消息 = 一个输入项"升级为"一条消息 = 一份表单(多输入项 + 校验规则)" | 决定 schema 是否升 `version 2`；影响 3.4/3.5 的设计与结果 `values` 契约 |
| D2 | 插件(agent 侧)是否允许自带 `ui`；client 侧插件是否允许注册 node 渲染器 | 决定 3.3 范围与插件 ABI 变更量 |
| D3 | `options` 的写入作用域(会话级/全局)与权限边界(谁能声明) | 决定 3.4 的安全模型 |
| D4 | stdio 勾选项的交互形态(行内勾选 vs 命令行附加参数) | 决定 3.1 的实现与测试形态 |
| D5 | 是否需要"能力协商"(Hello/HelloAck 版本交换) | 决定 3.6 是否立项 |
