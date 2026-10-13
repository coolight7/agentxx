# 修复 release 启动 agentxx_cli 崩溃 (插件 DLL 产物自相矛盾: LTCG 代码生成缓存)

- 难度: B
- 类型: bug修复
- 基于commit: 4b4b98f56b2a2e0dd4bb0ac1e5f7b7bd0e5e5cbc
- 时间: 2026-10-13 22:00
- 需求:
```md
请修复 release 启动 agentxx_cli 时崩溃
```

## 现象与根因

现象: `build/windows-release-output/agentxx_cli.exe`(以及 `build/windows-release/exec/agentxx_cli.exe`)
启动即崩, 退出码 `-1073741819`(0xC0000005 访问违例); debug 产物(`windows-debug-output`)一切正常。
崩点在插件 `agentxx_codegraph` 加载阶段, 最后一行日志是
`CodeGraphManager: reuse existing index db from parent path: ...`。

定位过程(全部在 release 产物上复现, debug 不复现):

1. `--help` / 纯参数解析正常, 崩在"加载插件 → 插件 start"这一段。
2. 用 cdb 挂上: 目标指令是插件里的 `call rax`, 此时 `rax = 0x0101010101010101`;
   数据来自 `.data` 里一个"惰性初始化 + 函数指针 + 上下文"结构, 而该结构由
   插件启动时从 `.rdata` 一张表整块拷贝(0x68 字节)而来 —— 也就是说**表里本该是
   函数指针, 实际是别的表的字节**(`01 01 00 00 00 00 01 00` + 16×`01` + …)。
3. 把坏产物和"同输入重新链接"的好产物逐字节对比: **`.text` 完全相同**,
   差的是 `.rdata` 里一段约 5.8 MB 的连续区段(表整体错位约 0x160 字节)与 `.reloc`。
   代码是 A 版布局、数据是 B 版布局 ⇒ 产物自相矛盾, 崩溃只是它的一种表现。
4. 判据(可直接核对同一处 RVA 的字节):
   - 坏: RVA `0x24B5DC0` 起是 `01 01 00 00 00 00 01 00 01 01 ...`, `--dump-config` 退出 -1073741819
   - 好: 同一处是函数指针表 `B0 CF 24 80 01 00 00 00 A0 D0 24 80 01 00 00 00 ...`
     (0x180000000 基址的相对地址, 加载时重定位), `--dump-config` 退出 0
5. 根因: release 用 `/GL`(编译) + `/LTCG`(链接), 链接命令行里带
   `/LTCGOUT:"<IntDir><TargetName>.iobj"`(MSVC 的 LTCG 代码生成缓存文件,
   由 WholeProgramOptimization / 链接任务自动带上)。该缓存会被**复用**:
   本轮实测"输入与命令行完全相同的两次链接"(一次沿用已有 `.iobj`, 一次先删掉它)
   产出 SHA256 不同、链接耗时都约 39 s —— 即 `.iobj` 的内容确实参与结果;
   当它陈旧/与刚变更的输入混用(当时 `libcxx_utilxx_base_static.lib` 刚在 17:56 重建,
   插件 17:58 重链; 同一时段仓库里还有另一进程在改代码/构建)时, 就可能把
   "旧布局的代码 + 新布局的只读数据"链在一起, 得到上面这种自相矛盾的 DLL。
   debug 无 LTO, 所以从来不受影响。

## 处理

1. 删掉该目标的 LTCG 缓存后重新链接(强制完整代码生成), 并把产物同步到
   `build/windows-release/exec/plugins/agentxx_codegraph/` 与
   `build/windows-release-output/plugins/agentxx_codegraph/`:
   ```
   del "<build>/agentxx_lib_repo-prefix/src/agentxx_lib_repo-build/agentxx_plugins_builtin/agentxx_codegraph/agentxx_codegraph.dir/Release/libagentxx_codegraph.iobj"
   del "<build>/exec/plugins/agentxx_codegraph/libagentxx_codegraph.dll"
   MSBuild <...>/agentxx_codegraph.vcxproj /p:Configuration=Release /p:Platform=x64
   ```
   (用构建脚本的环境变量 `VCPKG_ROOT=` / `VCPkgLocalAppDataDisabled=1`, 与脚本一致)
2. `agent/script/windows_release_build.bat` 增加**启动冒烟检查**: 安装与 CRT 打包后运行
   `agentxx_cli --dump-config`(初始化 libagentxx + 全部已配置插件后退出), 非 0 退出即
   报错退出并提示删除陈旧 `*.iobj` 重链; 可用 `AGENTXX_SKIP_SMOKE=1` 跳过。
   本次改动只加了这一段(26 行), 没有改任何业务源码 —— 源码本身没有问题。

## 验证

- release `agentxx_cli --dump-config` ×5 两个产物目录都是 0; `--dump-diagnostics` = 0;
  `jsonl` 模式(立即 EOF)= 0; `cli` 模式启动 15 s 无崩溃。
- `agentxx_codegraph` 插件正常加载: `agentxx_codegraph started (6 tools)`,
  5 个工具(search/context/callers/callees/path)注册成功。
- 修复后产物的 `.rdata` RVA `0x24B5DC0` 起是函数指针表(见上), 不是 0x01 填充。
- 冒烟检查的三个分支都在临时脚本里跑过: 正常→提示 OK 返回 0; exe 起不来→提示失败返回 1;
  `AGENTXX_SKIP_SMOKE=1`→跳过。

## 经验

- 遇到"release 启动即崩 / 某个插件加载时段错误", 先怀疑**构建产物本身**, 不要先怀疑源码:
  同一份源码在 debug 下正常、release 下崩, 且崩点在"代码从表里取函数指针"这类间接调用时,
  直接用 `agentxx_cli --dump-config` 复核退出码, 再对比产物字节(代码段的引用 vs 数据段的内容)。
- 仓库里有并发改动(或多入口/多目录同时构建)时, LTCG 的 `.iobj` 缓存与产物不一致的风险明显上升:
  该配置下 relink 之前先 `del /S /Q {build}\*.iobj` 是最省事的规避方式
  (代价只是本次链接做完整代码生成, 本插件实测约 39 s)。
- 不要用 `--dump-config` 之外的方式判断"插件能不能加载": 该命令正是"初始化 lib + 全部插件"的路径。
