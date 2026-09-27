# agent/js —— JS 插件用的界面 kit

这个目录放**给 JS 插件用**的脚本（生成物，改定义后跑 `agent/script/gen_ui_kit.ps1` 重新生成）。

| 文件 | 说明 |
|---|---|
| `agentxx_ui_kit.js` | agentxx 扩展 kit：插件界面描述层的组件装配函数，写全局 `pluginxx.ui.kit`。内容是**合并后的超集**（基础 kit 的全部组件 + agentxx 自己的留白口径与常用组合），只加载它一个就够 |

## 当前状态：JS 侧还没有界面入口

agentxx 的 JS 插件（`agentxx_javascript_engine` 的 `interpreter.js` 能力）桥接面是
**工具 / 钩子 / 事件 / 会话资源 / 定时器**（见引擎里的 `registerTool` / `onHook` /
`subscribe` / `addSkillDir` …），**没有界面注册入口**：JS 插件现在提交不了面板 / Info
段落 / overlay 的内容。所以这份 JS kit 目前是"就绪未接通"：

- 它生成的组件描述与 C++ 侧同一份定义（同一个生成器、同一套组件表），不会分叉；
- 等 JS 桥补上界面注册（或插件经自己的 C++ 壳提交：壳里 `setPanelJson` / `showItemsOverlay`
  收 JS 给的 JSON）之后，插件脚本直接可用：

```js
// plugin.js（JS 桥提供界面入口之后）
const kit = pluginxx.ui.kit;

// 不传 env：产出中立描述，由客户端渲染前自己适配（默认做法）
const row = kit.listRow({ title: '切歌次数', trailing: '3' });

// 传 env（客户端能力摘要）：kit 挑更合适的变体；能力摘要见客户端状态快照的 ui 段
const items = [
  kit.title({ text: '系统' }),
  kit.toolCallRow({ name: 'grep', depict: '在 src 下搜索 TODO', status: '完成', statusTone: 'success' }),
  kit.diagramBlock({ mermaid: 'stateDiagram-v2\n  [*] --> A' }),
];
```

## 装载方式（JS 引擎一个插件一个脚本）

`interpreter.js` 能力**每个插件只装一个脚本、各自一个 JS 上下文**，所以 kit 不能当第二个
脚本单独装载 —— 把它放在插件脚本之前即可：

1. **构建期拼接**（推荐）：插件自己的 `CMakeLists.txt` 用 `add_custom_command` 把
   `agentxx_ui_kit.js` 与 `plugin.js` 按顺序拼成一个发布用的脚本；
2. **粘贴**：直接把 `agentxx_ui_kit.js` 的内容放在 `plugin.js` 开头（脚本顶层是
   `(function (global) { … })(globalThis)`，不注册任何工具/钩子）。

## 纪律

- kit 只装配、不含逻辑；组件字段与适配规则见描述层生成的 `docs/ui-schema.md`；
- 尺寸只有一个单位 u（GUI 1u = 1 逻辑像素），终端客户端按自己的能力换算；
- 新增组件 / 改留白要改 `agent/schema/agentxx-ui-kit.def.json`，重新生成后把生成物一起提交。
