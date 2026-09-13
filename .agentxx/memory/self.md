## 自更新
- 现在你运行在`Agentxx`的 agent 项目上，当前 c++ Agent 项目代码就是你的 agent 源代码
- 默认使用的yaml配置文件是 `当前工作目录/agentxx-config.yaml`，默认使用的 .env 文件跟 `agentxx-config.yaml` 同目录
- 配置分层加载: overlay (工作目录或 --config 指定的 yaml + 同目录 .env) 覆盖 base
  (overlay 的 `data_dir` 目录下的 `agentxx-config.yaml` / `.env`; 未配置 data_dir 时取系统数据目录
  `~/.agentxx/`, 工作目录没有 yaml 时即直接加载该配置)
- 配置结构 (2026-09 起): `models`→`model.list`, `plugins`→`plugin.list`, `use_model`→`model.use`;
  列表段统一为 `{overwrite: {mode: merge|replace, remove: [...]}, list: [...]}` (纯列表写法已废弃),
  旧键/旧写法会被告警并忽略
- 两层的合并规则: 默认 `mode: merge` (按键归并/追加去重), `replace` 整段不继承,
  `remove` 按身份剔除 base 项 (model=name / plugin=path·name / mcp=namespace / 路径列表=字符串);
  其余键为标量覆盖、映射逐键合并; `.env` 同名变量取 overlay 值
- 如果运行中发现 tool 有问题、想增加新的 tool，可以跟用户沟通得到许可后，即可添加实现并编译、测试，然后提示用户重新运行即可
- win端编译脚本最后可能报错`copy agentxx_cli`到 `windows-debug-output` 失败，这可能是因为你自身程序 (Agentxx) 正在运行中占用了文件，请勿 kill 它，这个复制失败并不影响运行`windows-debug`内的程序和测试
- 使用 grep、glob 检索文件时，应当避免直接传入 `agent/**`, agent 文件夹内包含大量 build、third_party 代码和文件