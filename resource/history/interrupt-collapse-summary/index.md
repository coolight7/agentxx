- 难度: B+
- 类型: 功能 (中断消息展示优化 + 权限卡片特化)
- 时间: 2026-11-07
- 状态: 设计完成, 待实施 (plan.md 已写, work.md 全待完成)
- 需求 (用户原话):
```md
请了解当前代码实现，仔细思考分析，如何实现 中断提示消息显示优化，折叠时显示简略信息，
如果是 tool 申请权限，特化渲染 tool 名称、特化渲染权限名称，支持展开显示详情

调整: 
1. 所有中断在未处理时都固定展开不可折叠, 且开头的标签保留为 !, 处理后的中断都自动折叠,
   且标签回到 + -; 权限中断时纯摘要即可;
2. 权限名取 `读取文件 / 写入文件`;
3. 先规划完善的设计文档到 history
```

## 目标 (一句话)

中断消息的处理状态直接决定它的展示形态: **未处理 = `!` + 整份展开且不可折叠**,
**处理完成 = `+` 折叠成一行摘要 (可展开为 `-`)**, 权限询问的摘要里用工具特化显示名
(`agentxx_filesystem_write` → `Write`) 与本地化权限名 (`写入文件`)。

## 关键决策

1. 折叠**不是**用户对未处理消息的操作, 而是**状态驱动的默认形态**: `Waiting` 恒展开,
   `Confirmed`/`Canceled`/`Expired` 立即自动折叠 (之后用户可自行展开/收起);
   折叠行**保留处理结果** (`已确认: ...` / `已取消` / `已过期`), 不看展开也能知道结论。
2. 客户端仍然**零 permission 语义**: 摘要分段、权限名的 i18n 键、目标路径都由 agent 侧的
   `preset::permissionCard` 写进中断描述 (`InterruptUi.summary` + 分段的 `kind` 提示)。
3. 工具名特化**复用已有的工具特化渲染**: 客户端按工具名做一次"名称查询"
   (与生成占位提示 `genPlaceholderToolDisplayName` 同一路径, 缓存键 `#工具名`),
   显示名如 `Read`/`Write`; **未就绪时该段留空 (绝不先显示原名再跳变)**,
   没有注册渲染器的工具才显示原名 (结果不会再变)。
4. 折叠/展开的命中留在 `InterruptView` 内部 (不并入 `MessageListComponent` 的
   `handleCollapsibleClick`), 否则展开态的控件点击会被折叠判定抢走。
5. 标记列: 未处理 `!` (error), 已处理折叠 `+` / 展开 `-` (hint); 头行文案里重复的 `!`
   去掉, 屏幕上仍然是 `! [权限] ...` / `! [中断] ...`。
6. **权限名只靠颜色区分** (`filesystem_read` → `hint`, `filesystem_write` → `thinking`):
   不加粗 (加粗是全局排版语义, 不能当区分手段)、不加方括号/背景色; 收尾需专项核对。

## 影响文件

- `agent/lib/include/agentxx/middlewares/interrupt_ui.h` / `src/middlewares/interrupt_ui.cpp`
- `agent/lib/include/agentxx/middlewares/interrupt_presets.h` / `src/middlewares/interrupt_presets.cpp`
- `agent/client/include/agentxx-client/io/tui/components/interrupt_view.h` / `src/.../interrupt_view.cpp`
- `agent/client/src/io/tui/components/message_list.cpp` (工具显示名 resolver + 粗估高度)
- `agent/client/src/io/tui/agent_tui.cpp` (创建时的折叠值 + 过期时的自动折叠)
- `agent/client/src/io/tui/tui_i18n.cpp` (权限名 + 头行文案)
- 测试: `agent/test/core/test_interrupt_ui.cpp` / `test_interrupt_bus.cpp` /
  `agent/test/client/test_tui_interrupt.cpp`
- 文档: `docs/zh-cn/design/tui.md` §2.6

## 结论 (实施后回填)

(待实施)
