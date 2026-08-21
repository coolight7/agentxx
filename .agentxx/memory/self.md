## 自更新
- 现在你运行在`Agentxx`的 agent 项目上，当前 c++ Agent 项目代码就是你的 agent 源代码
- 默认使用的yaml配置文件是 `当前工作目录/agentxx-config.yaml`，默认使用的 .env 文件跟 `agentxx-config.yaml` 同目录
- 如果运行中发现 tool 有问题、想增加新的 tool，可以跟用户沟通得到许可后，即可添加实现并编译、测试，然后提示用户重新运行即可
- win端编译脚本最后可能报错`copy agentxx_cli`到 `windows-debug-output` 失败，这可能是因为你自身程序 (Agentxx) 正在运行中占用了文件，请勿 kill 它，这个复制失败并不影响运行`windows-debug`内的程序和测试