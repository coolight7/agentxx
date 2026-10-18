# diff overlay 支持多段 + `edits` 双形态的 UI 适配
- 难度: B
- 类型: 功能
- 基于commit: 9806ea68f4ce69509c354e179863fd0a7b52e402
- 时间: 2026-10-18 20:15
- 需求:
```md
很好，请继续修改，DiffOverlay 支持 单段、多段，另外 edits 字段对于 edit tool 的特化渲染、折叠特化渲染都应当支持 单结构体、[数组多结构体] 格式，因为toolcallNode会自动转换类型自适应，所有ui层也需要适配
```
- 实现要点:
    - diff overlay (`open_overlay DIFF`) 的 payload 从"只有单段"扩到单段/多段并存,
      段落形态与 edit 工具参数同一套"少写一层"规则 (工具参数的类型自动修正会把
      "单个对象"与"单元素数组"成对互转, 界面层不能只认一种):
        - 多段 `{"path":"a.cpp","edits":[{"old_str":"..","new_str":".."}, ...]}`
        - `edits` 为单个对象 (少写一层数组) 按单段处理
        - 单段旧写法 `{"path":"a.cpp","old_str":"..","new_str":".."}` 保留支持
        - 裸数组 `[{...}, ...]` 按"无路径的多段"处理
      解析唯一实现 `agentxx::client::parsePluginDiffPayload`
      (`agent/client/src/io/tui/plugin_ui_items.cpp`, 与 diff 渲染同一文件):
      给了 `edits` 就不再读 `old_str`/`new_str` (与工具参数的互斥规则一致); 段内缺
      字段按空串 (删除/纯新增那一侧); 两侧都为空的段跳过; payload 非法返回
      `std::nullopt` (调用方不打弹窗), 合法但没有可用段时渲染 "(no changes)"
    - `DiffOverlay` 改为持有**内容项表** (每段一个 `Diff` 项 + 段间一个 `Divider` 项),
      构造时按段落生成一次 (段落文本可能很大, 不逐帧复制); 渲染走共享组件层
      (`renderItems`), 因此逐行差异、side-by-side 判定、分隔样式与工具展开体/其它
      overlay 完全同源, 不再单独实现一份; 分隔用面性风格的浅色区块 (与 CUSTOM overlay
      Dividers 一致)。多段时文件路径只在首段展示
    - SDK 新增 `ClientPluginBase::showDiffOverlay(title, path, segments)` (头文件内联,
      C ABI/接口表零改动): 单段按旧写法下发 (老主程序也能渲染), 多段用 `edits` 下发;
      空段列表打开"无变化"的 diff
    - edit 工具特化渲染 (折叠头摘要 + 展开体) 原先已支持 `edits` 数组与单对象两种形态
      (`agentxx_fs_plugin::editSegmentsArg`), 本次补齐端到端测试把两种形态钉死: 单对象
      与"只含一项的数组"渲染结果逐字一致 (摘要、items dump、镜像渲染输出都比较)
    - 文档: `docs/zh-cn/design/plugins.md` §9.3 补 DIFF payload 四种写法与 SDK 便捷
      方法; `docs/en/design/plugins.md` 补同一段说明; `client_plugin_api.h` 的
      `AGENTXX_OVERLAY_DIFF` 枚举注释写全形态
    - 测试: `tui_surface` (payload 解析 8 类形态 + 单段/多段/单对象/空/非法 payload 的
      离屏渲染上屏断言, 含段间分隔行与"路径只出现一次"), `client_plugins` (真实插件
      单对象形态渲染、与数组形态一致性、SDK showDiffOverlay 的 payload 组装),
      `tui_tool_header` (单对象形态展开体与数组形态逐字一致)
