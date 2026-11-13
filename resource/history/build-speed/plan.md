# 编译速度优化 — 方案与基线

## 背景

Windows Debug 的日常构建 (改一个文件 → 出可运行的 `agentxx_test` / `agentxx`) 偏慢。
先把时间花在哪里测出来, 再决定改什么。

## 现状调查 (实测基线)

机器: 12 核 / 52 GB, VS18 (MSVC 14.51), 生成器 "Visual Studio 18 2026", Debug 配置
(ASan 默认 ON, `/Z7`, `/INCREMENTAL:NO`), 未安装 ccache。

| 场景 | 耗时 |
| --- | --- |
| 顶层 no-op (`cmake --build build/windows-debug --parallel 12`, 什么都没改) | 60.5 s |
| 内层 lib no-op (全部 up-to-date) | 25.1 s |
| 内层 lib install (只打印 Up-to-date) | 13.2 s |
| 内层 test no-op | 18.6 s |
| 内层 client no-op | 11.8 s |
| 改 1 个小 TU (`task_scope.cpp`, 3.3 KB), 只编 `agentxx_static` | 17.0 s (其中 CL 任务 11.45 s) |
| 改 1 个大 TU (`event_stream.cpp`), 只编 `agentxx_static` | 24.6 s |
| 同样改大 TU, 只编 `agentxx_shared` | 24.2 s |
| 改大 TU, 编 ALL (静态 + 动态) | 29.6 s |

内层 lib no-op 的 MSBuild 任务耗时 (PerformanceSummary, `/clp:PerformanceSummary`):

| 任务 | 耗时 | 说明 |
| --- | --- | --- |
| MSBuild (嵌套工程) | 15.2 s | 114 次调用 |
| ResolveProjectReferences | 10.9 s | 29 次 |
| PostBuildEvent | 8.4 s | 29 次 (插件 `plugin.yaml` 复制, 每次构建都跑) |
| Exec | 8.5 s | 26 次 (插件资源 `copy_if_different`) |
| ComputeStdModulesCompileInputs | 5.0 s | 27 次 —— **模块扫描, 本项目不用模块** |
| GetLibraryModuleSources | 5.0 s | 27 次 —— 同上 |
| GetCopyToOutputDirectoryItems | 2.2 s | |
| LIB (静态库链接) | 3.4 s | 107 个 .obj |

两个结构性问题:

1. **MSVC 下同一批源文件编译两遍**: `lib/CMakeLists.txt` 的 MSVC 分支同时建
   `agentxx_shared` 与 `agentxx_static`, 各自编译全部 107 个 TU (dllexport 语义不同,
   不能像 GCC/Clang 那样共用 OBJECT 库); 而 client / test / benchmark 只链静态库。
2. **`xx:latest` + MSVC 的模块扫描**: 每改一次就要重新做一遍, 纯浪费 (不用 `import std`)。

## 方案

1. `-DCMAKE_CXX_SCAN_FOR_MODULES=OFF`
   - 顶层 `_AGENTXX_COMMON_CMAKE_ARGS` 里加一行 (覆盖全部嵌套工程: 三库/插件/第三方)
   - lib / client / test 自己的 CMakeLists 也显式关掉, 单独构建这些目录时同样生效
2. MSVC 启用 PCH: PCH 头列表提取为变量, GCC/Clang 与 MSVC 共用同一份;
   只预编译稳定第三方头 (std / fmt / asio / boost.exception), 项目自身头不参与
3. 新增 `AGENTXX_LIB_BUILD_SHARED` (默认 ON): OFF 时不创建/不安装/不导出 `agentxx_shared`
   (静态库与 `agentxx_static` 的导出配置保持不变); Debug 脚本默认关掉, 需要 FFI/嵌入产物时
   显式 `-DAGENTXX_LIB_BUILD_SHARED=ON`
4. 插件资源复制 (PostBuildEvent / Exec 合计 ~17 s/次) 本轮不动: 要改 25 个插件
   CMakeLists, 收益与风险不对等 (记录在 work.md 的"可继续优化"里)

## 边界与风险

- PCH 只在 MSVC 上新增; 若出现奇怪的重定义/顺序问题, 用 `-DAGENTXX_ENABLE_PCH=OFF` 回退
- `AGENTXX_LIB_BUILD_SHARED=OFF` 时 `plugin_cleanup` 用例里"非插件动态库"子项会被跳过
  (它按名字找 `libagentxxd.dll`, 找不到时打印 INFO 跳过) —— 属于预期降级
- 不改生成器 (VS → Ninja): 脚本依赖 `-A x64` 自动发现工具链, 换生成器要另加 vcvars 前置

## 验证

- 同一台机器、同一构建目录, 逐项改前后各测同一批场景 (no-op / 小 TU / 大 TU / 头文件改动)
- `agentxx_test` 的 `event_bridge` `view_replica` `plugin_cleanup` `boundaries` 等模块跑通
- `AGENTXX_LIB_BUILD_SHARED=OFF` 能正常 configure + build + install (`agentxx_test` 可运行)
