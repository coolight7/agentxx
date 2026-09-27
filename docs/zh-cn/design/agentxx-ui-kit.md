# 扩展 kit（agentxx）（v1）

> 本文件由 `tools/gen_ui.dart` 生成。

纪律：① 只写数值单位 u（8 / 12 / 20 这类）；② 不引用客户端专属块；③ 不含逻辑（只装配）。
需要项目特有的间距口径时，由扩展 kit 覆盖同名组件实现。

| 组件 | 参数 | 说明 |
|---|---|---|
| `title` | `text`* | 标题行 |
| `hint` | `text`* | 次要说明行 |
| `text` | `text`* `tone`=normal `mono`=false | 正文行 |
| `badge` | `text`* `tone`=accent | 状态小标签 |
| `icon` | `name` `glyph` `size` `tone`=normal | 图标（目标不支持 Icon 时退化成 glyph 文本） |
| `gap` | `size`=gap | 竖直留白（缺省用客户端默认行距） |
| `divider` | — | 分隔线 |
| `button` | `label`* `variant`=secondary `icon` `disabled`=false `action` | 按钮 |
| `actionsRow` | `buttons`* | 一排等宽按钮（按钮列表里的每一项占一等份） |
| `card` | `title` `variant`=card `padding`={horizontal: 8, vertical: 4} `margin` `children` | 内容块（覆盖基础 kit：终端卡片的留白更紧） |
| `listRow` | `title`* `subtitle` `trailing` `action` | 卡片里的一行（覆盖基础 kit：终端行更紧，右侧文字用说明色调） |
| `section` | `title`* `rows`* | 小节标题 + 若干行 |
| `kv` | `pairs`* `sep` | 键值块（键列按最长键自适应） |
| `table` | `columns`* `rows` `header`=true | 表格（未指定的列宽由客户端自动分配） |
| `tree` | `nodes`* `connector`=true | 层级列表 |
| `sparkline` | `data`* `height`=1 `glyphStyle`=block `showLast`=true `tone`=accent | 迷你趋势图 |
| `progressRow` | `label` `value`* `total`=100 `unit`=% | 一行进度（覆盖基础 kit：agentxx 默认按百分比显示数值） |
| `toolCallRow` | `name`* `depict` `status` `statusTone`=hint `action` | 工具调用行：工具名 + 一行摘要 + 可选状态标签 |
| `thinkingBlock` | `content`* `label` | 思考 / 说明段落：可选小标题 + 次要色调的正文 |
| `sessionStats` | `title` `columns`* `rows`* | 统计表：可选标题 + 表格（列宽由客户端自动分配） |
| `pathDiffRow` | `path`* `oldStr`* `newStr`* `title` | 路径差异：可选标题 + 差异对比（目标不支持 Diff 时退化成等宽文本） |
| `diagramBlock` | `mermaid`* `title` | 状态图：可选标题 + mermaid 描述（目标不支持 Diagram 时退化成等宽源码） |
| `interruptRow` | `confirmLabel`* `cancelLabel`* `confirmAction`=__submit `cancelAction`=__cancel | 确认 / 取消行（动作名缺省用中断域内约定：__submit / __cancel） |

按格换算的辅助函数（用客户端 `cell`，缺省取 `defaults.cell`）：

