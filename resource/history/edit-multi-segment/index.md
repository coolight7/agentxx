# edit 工具多段替换 + TUI 多段 diff 渲染 + 参数类型自动修正 (对象数组)
- 难度: B
- 类型: 功能
- 基于commit: 9806ea68f4ce69509c354e179863fd0a7b52e402
- 时间: 2026-10-18 16:58
- 需求:
```md
请修改 edit tool 支持多段 [{old_str, new_str}, {old_str, new_str}, ...]，并修改 tui 的特化渲染也支持多段，另外检查 toolcallNode 的自动修正参数 是否支持 List Object 与 Object 的类型自动互相转换
```
- 实现要点:
    - edit 工具 (`agent/plugins/agentxx_filesystem/`) 新增 `edits` 参数: 元素为
      `{"old_str": "...", "new_str": "..."}` 的数组, 按顺序应用 (后一段作用于前一段
      改完的内容, 可匹配前一段写入的文本); 任一段匹配不到即整次调用失败且不写盘,
      错误提示指明是第几段。`old_str`/`new_str` 保留为单段写法, 与 `edits` 同时给出时
      报错; 两段写法都要求 `new_str` 明确给出 (删除请传空串, 避免漏写字段变成误删)。
      结果文本: 单段沿用 `success` / `Success, Replace N hits`; 多段为
      `Success, Replace 总处数 hits in 段数 segments: h1 + h2 + ...`
      (`agentxx_fs_plugin::parseEditSegmentHits` 按段解析处数, 段数/和对不上时不换算)
    - 同步 (`fileEditExecuteImpl`) 与协程 (`fileEditExecuteAsyncImpl`) 两个执行体共用
      同一套 helper (`parseEditSegments` / `applyEditSegments` / `editResultText`)
    - TUI 特化渲染 (插件 client 段 `registerRenderer(kNameEdit)`): 折叠头摘要按各段
      增删行数求和 (`diffStatTextOfSegments`), `multi_replace` 完成后按结果里的
      每段处数换算; 展开体每段一个 `Diff` 组件项 (路径只在首段), 段间插 `Divider`
      —— 界面描述层的 Diff 组件只有一段 `oldStr`/`newStr`, 故多段用多个组件项表达,
      渲染层无需改动 (各段各自渲染一份逐行差异, 高度估算按项汇总)
    - 工具模式: 新增插件 SDK helper `ToolSchemaBuilder::objectArray` (声明"元素是对象"
      的数组参数); edit 的 `edits` 用它声明 `items.properties/required`
    - `ToolcallWrapNode::autoFixArgsType` 原先不支持对象与对象数组互转, 本次补齐:
      object -> `[object]` (目标声明数组且元素为对象)、`[单对象]` -> object (目标声明
      object); 同时修掉解包时"来源与被赋值对象同属 args[name] 导致赋值先释放来源"的
      隐患 (先拷出元素再赋值)
