# 插件接口表版本与兼容机制 —— 实施记录

> 对应方案: 同目录 [plan.md](plan.md) (设计 v3.4)
> 分支: `main` | 编译: `agent/script/windows_debug_build.bat` (Debug, ASan+UBSan)
> 提交按 plan.md §6 的 10 个提交推进, 每步编译 + 跑相关测试模块。

## 进度总览

| 提交 | 内容 | 状态 | 提交号 |
|---|---|---|---|
| 1 | 机制层: `iface_meta.h` + `kit.h` 判定 + 10 张通用表常量 | 已完成 | `cxx_pluginxx@8c7dfba` |
| 2 | agent/client 表头四件套 + `plugin_iface_meta.h` 特化 | 已完成 | `c054066d` |
| 3 | `plugin_spec.h` + 9 个跨边界结构体补 `struct_size` + 插件级 `api_version` 闸门 | 已完成 | `b40ae068` (框架 `c953365`) |
| 4 | `hooks_ex` 折回 `hooks` | 已完成 | `7744ad29` |
| 5 | 自检与测试 (boundaries 四件套 + 偏移断言 + 单测) | 已完成 | `6df3a3ee` (框架 `aa93059`) |
| 6 | 文档与注释清理 (`plugins.md` §8/§9、`AGENTS.md`) | 已完成 | `3f721e4c` |
| 7 | cxx_* 三个库文档/注释措辞统一 | 已完成 | `e8d83af1` (框架 `a6b5fea`/`9112bb7`/`77b0379`/`ba533cc`) |
| 8 | agentxx 插件相关源码注释措辞统一 | 已完成 | `e892cb06` + `81d10005` |
| 9 | agentxx 文档 + `docs/en` + 根 `AGENTS.md` | 已完成 | `3ed9d38f` |
| 10 | 黑话清理 + `boundaries` 称呼/黑话规则 + 全量回归 | 已完成 | `adf1c30d` (框架 `809c8df`) |
| 11 | 开头写清三层关系 (主程序 / 插件框架 / 动态库插件 / JS 插件) | 已完成 | 本记录所在提交 |

## 提交 1: 机制层 (cxx_pluginxx)

- 新增 `include/pluginxx/api/iface_meta.h`:
  - `pluginxx::IfaceMeta<Iface>` (最低兼容版本 + 基线字节数), 10 张通用表各一条特化;
  - `ifaceHas<&表::成员>(t)`: 表长是否覆盖某成员 (追加成员的唯一判据);
  - `ifaceHasVersion(t, n)`: 语义档位判定;
  - `memberEndOffset<Member, Spec>()`: 取"成员结束偏移"(探针对象取地址相减, 不依赖
    `offsetof` 只能吃成员名字的限制, 也不走空指针);
  - 每条特化后跟两条 `static_assert` (基线 >= 8 且 <= 完整表长)。
- `tables.h`: 10 张通用表各补 `_MIN_VERSION` 与 `_BASELINE_SIZE`, 每张表的 `version`
  字段注释改为"主程序填 `_VERSION`"; 文件头新增"接口表的版本与布局规则"整段
  (四件套 / 判定规则 / 改动规则); `capabilities` 表加"成员顺序即契约"说明。
- `kit.h`: `validateInterface` 改用 `IfaceMeta` —— `version < min_version` 或
  `struct_size < baseline_size` 才拒绝 (修掉写死 `!= 1` 导致"一张表升版连累所有表")。
- 测试: `test_plugin_runtime.cpp` 的"接口表协商"用例改为新语义 (更高版本 + 完整长度
  = 接受; 低于最低版本 = 拒绝; 表长不足基线 = 拒绝; 表长等于基线 = 接受)。

验证: debug 构建通过; `agentxx_test boundary plugin_runtime plugin_sdk plugin_hooks`
958 项全绿。

## 提交 2: agent/client 表头四件套

- `plugin_api.h`: 11 张 agent 表 (含待折回的 `hooks_ex`) 补 `_MIN_VERSION` /
  `_BASELINE_SIZE`; 文件头"版本策略"一行改写; 表尾在 `__cplusplus` 下自动包含
  `plugin_iface_meta.h`。
- `client_plugin_api.h`: 9 张 client 表同上; 文件头"版本策略"段改写;
  ui 表"通用交互"段注释删掉"老宿主同位置为空指针"与 `(v3)` 标记, 改为"本段属于首版
  布局 (= 基线), 只判空即可"。
- 新增 `agent/lib/include/agentxx/plugin/api/plugin_iface_meta.h`: 20 张表
  (10 agent + 10 client) 的 `IfaceMeta` 特化 + 自检, 由两个 ABI 头自动包含。
- `plugin_kit.h`: 引入 `ifaceHas` / `ifaceHasVersion` 到 `agentxx::plugin`。

验证: debug 构建通过; 同上 958 项全绿 (含 `boundaries` 的接口表名数量规则)。

## 提交 3: 跨边界结构体统一带 struct_size

- 新增 `agent/lib/include/agentxx/plugin/api/plugin_spec.h`:
  - `SpecBaseline<Spec>` (缺省 = `sizeof(Spec)`, 14 个结构体各一条特化 + 两条自检);
  - `specSizeOk<Spec>(struct_size)`: `0` (基线布局) 或 `>= 基线` 接受, 其余拒绝;
  - `specHas<&Spec::字段>(struct_size)`: 追加字段可读判定 (复用框架的
    `pluginxx::memberEndOffset`)。
- 9 个结构体补齐 `struct_size`:
  - 零成本 (用原 `_reserved` 的位置, 大小与偏移不变):
    `AgentxxPluginHookSpec` (struct_size + point),
    `AgentxxToolRenderSpec` / `AgentxxTimerSpec` / `AgentxxKeybindSpec` /
    `AgentxxToolRenderInput` / `AgentxxUiActionContext` (struct_size + version);
  - +8 字节 (插入 `struct_size`): `AgentxxPluginToolSpec` (首字段 + `_reserved`),
    `AgentxxPluginGraphNodeTypeSpec` (首字段 + `_reserved`),
    `AgentxxOverlaySpec` (version 之后)。
  - 已有 `struct_size` 的 5 个结构体只改注释: "传 0 时按当前布局解析" → "0 = 基线布局"。
- 判定力度统一 (同一契约一种写法): 3 处旧检查 (`register_tool_permission` /
  `check_paths` / `register_hook_ex`) 改为 `specSizeOk`; 新增 2 处
  (`defineFeaturePoint` / `registerFeatureImpl`); 另外给 `register_tool` /
  `register_hook` / `register_node_type` / `register_tool_renderer` / `open_overlay` /
  `set_timer` / `register_keybind` 补上长度门槛。
- 填充方填长度: SDK (`plugin_kit.h`) 的 tool / hook / graphNode / timer / keybind /
  overlay / toolRender 构造函数各填 `sizeof`; 主程序侧 `AgentxxToolRenderInput` (2 处)
  与 `AgentxxUiActionContext` (1 处) 填 `sizeof`; `agentxx_planning` /
  `agentxx_javascript_engine` 的两个 spec 也填上。
- 插件级闸门统一: client 侧改为 `api_version >= AGENTXX_CLIENT_PLUGIN_MIN_API_VERSION`
  (新增该宏); 框架 `lifecycle.h` 内置插件的判据由 `!= PLUGINXX_API_VERSION` 改为
  `< PLUGINXX_MIN_API_VERSION` (与动态库插件一致, 修掉 D6 的另一半)。
- 注: agent 侧动态库插件的检查(`lifecycle.h`)本来就已存在 (方案里的 D6 描述已过时),
  本次只把内置分支的判据对齐。

验证: debug 构建通过; `plugin_runtime`/`plugin_sdk`/`plugin_hooks`/`plugin_feature`/
`plugin_cleanup`/`plugin_multi_instance`/`boundaries`/`assembly_snapshot` 1306 项,
`plugins` + `client_plugins` 1208 项, 全绿 (ABI 对照用例 C17 ↔ C++ 双向一致)。

## 提交 4: hooks_ex 折回 hooks

- `plugin_api.h`: 删除 `AgentxxPluginHooksExIface` 表与四个 `..._HOOKS_EX_*` 宏;
  `AgentxxPluginHooksIface` 表尾追加 `register_hook_ex` / `unregister_hook_ex` /
  `list_hooks` 三成员, 表注释合并两组能力的说明 (基线 = 完整表长, 三成员同为 v1 布局,
  不需要 `ifaceHas`); `AgentxxPluginHookSpecEx` 结构体保留 (它是入参结构体, 与表无关)。
- `plugin_iface_meta.h`: 删 `IfaceMeta<AgentxxPluginHooksExIface>` 特化。
- `plugin_kit.h`: `AgentIfaces::hooksEx` 删除; `hookEx` / `unhookEx` / `listHooks` 改为
  单路 (走 `iface.hooks`, 不再有"表缺失回退基础钩子"的双路分支)。
- `plugin_interfaces.h`: 删 `AgentHooksEx` 常量 (注释并入 `AgentHooks`)。
- `plugin_manager.h`: `kInterfaceTableCount` 10+11 → 10+10; 相关注释表名更新。
- `plugin_manager_vtable.cpp`: `g_ifaceHooksEx` 删除, 三入口移入 `g_ifaceHooks`;
  `query_interface` 去掉 `hooks_ex` 分支。
- `example_feature` (源码 + `plugin.yaml`): 接口名与说明改为 `agentxx.agent.hooks`。
- 测试: `test_plugin_hooks` 的表名/断言主体替换 (156 项全绿);
  `boundaries` 规则 8/10 的数量 21 → 20、agent 表数量 11 → 10。
- 文档: `plugins.md` §8 (两行并一行, 数量 21 → 20) 与根 `AGENTS.md` 的接口表数量与
  钩子小节先行更新 (完整 §9 重写见提交 6)。

验证: debug 构建通过; `boundaries` 13 项、`plugin_hooks` 156 项、`plugin_runtime` 674 项、
`plugin_sdk` 115 项、`plugin_feature` 132 项、`plugin_cleanup` 99 项、
`plugins` 551 项、`client_plugins` 657 项、`assembly_snapshot` 37 项全绿 (共 2434 项)。


## 提交 5: 自检与测试

- `boundaries` 新增两条规则 (规则 13/14):
  - **四件套完整性**: 从三份表头收集表结构体与 `IID` / `X_VERSION` / `X_MIN_VERSION` /
    `X_BASELINE_SIZE` 常量, 断言各项数量与表数量相等、`MIN_VERSION <= VERSION`、
    基线指向本组表、`IfaceMeta` 特化数 = 表数 = 29;
  - **成员顺序台账**: 从表结构体定义体按声明顺序抽**顶层**函数指针成员 (括号深度必须为 0,
    表内嵌套的回调参数不算), 与台账逐表比对 —— 防重排 / 防删除 / 防改名。
- `plugin_runtime` 新增两组用例:
  - 机制层: `ifaceHas` 三态 (NULL / 只覆盖基线 / 覆盖追加成员)、`ifaceHasVersion` 两态、
    `specSizeOk` 三态、`specHas` 两态 (用一张"基线 + 一个追加成员"的伪造表);
  - 布局冻结: 9 个跨边界结构体的关键字段偏移 (6 个零成本改动 / 3 个 +8), 64 位下另断言
    绝对大小 (96/64/64/32/56/56/56/96/56)。
- `plugin_feature` 新增"长度门槛"用例: 声明点与登记实现两处的 `structSizeOk` 正反例
  (低于基线拒绝且不落注册表, `0` 与 `sizeof` 接受)。
- 框架侧 `pluginxx::memberEndOffset` 拆出 `memberEndOf` (成员指针形态), 供结构体长度判定复用。

验证: debug 构建通过; `boundary` 13 / `plugin_runtime` 734 (+60) / `plugin_feature` 142 (+10)
/ `plugin_sdk` 115 / `plugin_hooks` 156 / `plugin_cleanup` 99 / `assembly_snapshot` 37 全绿。

## 提交 6: 文档

- `docs/zh-cn/design/plugins.md` 新增 **§17 接口表版本与基线 (改动规则)**:
  四种机制分工、四件套要求、判定规则、追加策略、跨边界结构体规则、
  操作手册 ("以后怎么改" / "永远不要做")、验证矩阵;
  §8 表后加四件套与 §17 的指引; §9 的"版本约定"段改为指向 §17。
- 根 `AGENTS.md`: 接口表数量 21 → 20、钩子小节改单表、新增"接口表版本与基线"小节。
- `docs/zh-cn/design/index.md`: 附录 B 的接口表数量与四件套说明、目录树里的表数量。
- `resource/history/plugin-share-feature/work.md`: 阶段 5 处补一句"`hooks_ex` 已折回"
  (只追加说明, 不改历史正文)。

## 提交 7~10: 术语统一

- 主体替换 (180 文件 / 1480 处): `宿主` → 主程序; FFI 场景 → 嵌入方程序;
  `AgentHost` → agent 节点容器; `EventHost` → 事件总线持有者;
  `HostTool` / `WireHostTool*` → 外部注册工具 (标识符与协议名不动)。
- 直译词: `装载`/`载入` → 加载; `领域` → 业务 (标识符行保留);
  `承载` → 保存/包含/负责; `载体` → 内容/信号; `编排` → 调度; `接缝` → 扩展点/对接点;
  `链路` → 流程; `视角` → 角度; `维度`/`层面` → 方面; `回灌` → 写回;
  非算法的 `拓扑` → 结构/顺序。
- 黑话: `口径` → 规则; `落地` → 实现; `埋点` → 扩展点; `门禁` → 校验;
  `颗粒度` → 划分粗细。`收敛` 逐条看过 —— 现存用法都是"收敛到某个结果"(数学/过程语义),
  保留; `兜底` / `对齐` 按方案保留。
- `基座` / `框架内核` / 指代插件框架的 `内核` → 插件框架; 操作系统内核含义保留。
- **`boundaries` 新增规则 15 "称呼与用词"**: 逐行检查停用称呼与直译词,
  `内核` 只允许操作系统含义 (内核对象/内核态/内核接口/内核时间/内核累计/内核过旧/
  内核就绪事件…), `领域` 只允许标识符行, `拓扑` 只允许算法名;
  一行出现两个及以上停用词判为"禁用词表本身"豁免, 规则文件自身与
  `resource/history/**` 不检查。
- 过程中发现并修复脚本误伤: 逐条替换脚本因 PowerShell 数组展平把"多字串"当成"首字符"
  替换, 污染了 14 个文件; 已 `git checkout` 复原后改用 "路径|||旧串|||新串" 纯字符串配对,
  并加了"括号数量与 HEAD 对比"的校验。

验证 (全量回归): `agentxx_test` 全模块 **37602 项全绿** (含 `boundaries` 14 项)。
`wire_schema` 用例里一处外部工具描述由"宿主编辑器"改为中性写法, 生成物
`agent/schema/wire-schema.json` 同步重生成。

## 提交 11: 三层关系写进文档开头

术语统一后"主程序"这个词在不同文档里含义不同 (agentxx / 使用插件框架的任意程序),
容易读混, 所以在两个入口处补三行说明:

- `docs/zh-cn/design/plugins.md` §1 总览首段: agentxx 是主程序; 插件框架由 cxx_* 库实现;
  插件分动态库插件与 JS 运行时插件加载的 JS 插件; 本文档的"主程序"都指 agentxx,
  cxx_* 库文档里同一词是通用含义。
- 根 `AGENTS.md` "## C++插件开发" 首行: 同一段说明 (内含 `agentxx_javascript_engine` 名字)。

验证: 纯文档改动, 未重编; 直接跑 `agentxx_test boundaries` -> 14 项全绿
(新增文字不含停用称呼, 规则 15 不触发)。

