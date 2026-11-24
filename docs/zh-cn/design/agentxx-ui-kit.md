# 扩展 kit（agentxx）（v2）

> 本文件由 `tools/gen_ui.dart` 生成。定义来源：`schema/ui.def.json` / `schema/kit.def.json`；扩展 kit 定义：`agent/schema/agentxx-ui-kit.def.json`。

纪律：① 只写数值单位 u（8 / 12 / 20 这类）；② 不引用客户端专属块；③ 不含逻辑（只加载）。
需要项目特有的间距规则时，由扩展 kit 覆盖同名组件实现。

| 组件 | 参数 | 说明 |
|---|---|---|
| `title` | `text`* | 标题行 |
| `hint` | `text`* | 次要说明行 |
| `text` | `text`* `color`=normal `mono`=false | 正文行 |
| `badge` | `text`* `color`=accent | 状态小标签 |
| `icon` | `name` `glyph` `size` `color`=normal | 图标（目标不支持 Icon 时退化成 glyph 文本） |
| `gap` | `size`=gap | 竖直留白（缺省用客户端默认行距） |
| `divider` | — | 分隔线 |
| `button` | `label` `variant`=normal `highlight`=false `icon` `glyph` `color` `size` `disabled`=false `action` | 按钮（不写 label 就是纯图标按钮，图标与文字都没有时用 glyph 当文字；color 定内容色，会盖住选中 / 禁用态） |
| `themeScope` | `child`* `data`* | 子树样式作用域：包住一块内容，data 由客户端定义（musicxx 用 text / surface / button / mask / shimmer 五组颜色）；客户端不支持时只给内容，样式不生效 |
| `actionsRow` | `buttons`* | 一排等宽按钮（按钮列表里的每一项占一等份） |
| `card` | `title` `variant`=card `padding`={horizontal: 8, vertical: 4} `margin` `child` | 内容块（覆盖基础 kit：终端卡片的留白更紧） |
| `listRow` | `title`* `subtitle` `trailing` `action` | 卡片里的一行（覆盖基础 kit：终端行更紧，右侧文字用说明色调） |
| `section` | `title`* `rows`* | 小节标题 + 若干行 |
| `kv` | `pairs`* `sep` | 键值块（键列按最长键自适应） |
| `table` | `columns`* `rows` `header`=true | 表格（未指定的列宽由客户端自动分配） |
| `tree` | `nodes`* `connector`=true | 层级列表 |
| `listItem` | `id` `title`* `subtitle` `trailing` `image` `icon` `color`=normal `selected`=false `action` | 列表项（数据对象，给 ListViewBase / ListView 的 items 用） |
| `menuItem` | `title`* `icon` `action` | 菜单项（数据对象，给 ListView 的 menu / itemMenu 用） |
| `sparkline` | `data`* `height`=1 `glyphStyle`=block `showLast`=true `color`=accent | 迷你趋势图 |
| `progressRow` | `label` `value`* `total`=100 `unit`=% | 一行进度（覆盖基础 kit：agentxx 默认按百分比显示数值） |
| `slot` | `slot`* `args` `fallback` | 在这里渲染一个槽位的内容（应用预留的或插件声明的）；没写 fallback 时不占位 |
| `textVar` | `text`* `color`=normal | 一段带变量的文本（字符串里的 {变量 id} 由客户端绑定） |
| `varValue` | `name`* `fallback` `unit` | 把某个变量绑定成值（`{"var": …}`），可用在尺寸 / 进度 / 开关等值位置 |
| `toolCallRow` | `name`* `depict` `status` `statusColor`=hint `action` | 工具调用行：工具名 + 一行摘要 + 可选状态标签 |
| `thinkingBlock` | `content`* `label` | 思考 / 说明段落：可选小标题 + 次要色调的正文 |
| `sessionStats` | `title` `columns`* `rows`* | 统计表：可选标题 + 表格（列宽由客户端自动分配） |
| `pathDiffRow` | `path`* `oldStr`* `newStr`* `title` | 路径差异：可选标题 + 差异对比（目标不支持 Diff 时退化成等宽文本） |
| `diagramBlock` | `mermaid`* `title` | 状态图：可选标题 + mermaid 描述（目标不支持 Diagram 时退化成等宽源码） |
| `interruptRow` | `confirmLabel`* `cancelLabel`* `confirmAction`=__submit `cancelAction`=__cancel | 确认 / 取消行（动作名缺省用中断域内约定：__submit / __cancel） |

按格换算的辅助函数（用客户端 `cell`，缺省取 `defaults.cell`）：

