# 编译速度优化 — 实施记录

## 进度

- [x] 基线测量 (no-op / 单 TU / 头文件改动 / PerformanceSummary)
- [x] 关闭 C++20 模块扫描
- [x] MSVC 启用 PCH
- [x] MSVC 静态/动态库共用同一批 .obj (此前编两遍)
- [x] 源文件拆成多个 OBJECT 工程并行编译 (lib / test / client)
- [x] 构建脚本并行度默认 = 核数
- [x] 验证: dumpbin 导出面、event_bridge/view_replica/plugin_cleanup/boundaries、client 可运行
- [x] 更新 AGENTS.md 编译规则

## 每一步改了什么

### 1. 关闭 C++20 模块扫描

- `agent/CMakeLists.txt`: `_AGENTXX_COMMON_CMAKE_ARGS` 加 `-DCMAKE_CXX_SCAN_FOR_MODULES=OFF`
  (覆盖全部嵌套工程); `agent/lib|client|test/CMakeLists.txt` 各自也 `set(... OFF)`,
  这样单独构建这些目录同样生效
- 效果: 内层 lib 一次 no-op 构建 25.1 s → 9.0 s (PerformanceSummary 里
  `ComputeStdModulesCompileInputs` 5.0 s + `GetLibraryModuleSources` 5.0 s 消失)

### 2. MSVC 启用 PCH

- `agent/lib/CMakeLists.txt`: PCH 头列表提取为 `_AGENTXX_PCH_HEADERS`, 与 client/test 共用;
  MSVC 分支过去完全不用 PCH (只有 GCC/Clang 用)
- `agent/cmake/agentxx_split_sources.cmake` 的 `agentxx_apply_pch()` 统一挂到真正编译源文件的目标上
- 效果: 单 TU 的 CL 任务 11.45 s → 10.15 s; 全量重建折算到每 TU 约 10.4 s → 8.0 s

### 3. MSVC 静态库/动态库共用同一批 .obj

- 过去 MSVC 分支 `add_library(agentxx_shared ...)` 与 `add_library(agentxx_static ...)`
  各自编译全部源文件 (担心 dllexport 语义差异, 见原注释)
- 实测导出标记是 `ffi_api.h` 里**无条件**的 `__declspec(dllexport)` (与目标无关),
  `AGENTXX_EXPORTS` 宏在代码中已无任何引用, 两端编译参数完全一致 → 改为与 GCC/Clang
  一样的 `agentxx_obj` OBJECT 库 + 两个产物
- 验证: `dumpbin /exports libagentxxd.dll` 的 `agentxx_ffi_*` 符号与 `ffi_symbols.map`
  逐项一致 (35/35, Compare-Object 无差异)
- 注意: `strdup`/`strcasecmp` 等 POSIX 兼容宏必须挂在 obj 目标上 (才是真正编译的地方),
  共享/静态库上保留只为继续向消费方传播

### 4. 源文件拆成多个 OBJECT 工程 (关键收益)

- 新增 `agent/cmake/agentxx_split_sources.cmake`:
  `agentxx_split_object_sources()` / `agentxx_inherit_usage_requirements()` / `agentxx_apply_pch()`
- lib: `agentxx_split_object_sources(agentxx 8 ...)` (14 个工程) ——
  替换掉此前的"单工程 107 个 TU"
- test: 三处 `target_sources(agentxx_test ...)` 改成累到 `agentxx_test_SOURCES`,
  文件末尾一次性拆分 (20 个工程) 并挂 PCH; 编译/链接要求用生成器表达式
  (`$<TARGET_PROPERTY:agentxx_test,...>`) 拷到 object 目标, 不必改写中间那几十个 target_* 调用
- client: 同样在文件末尾拆分 (5 个工程)
- 开关 `AGENTXX_MSVC_SPLIT_OBJECTS` (顶层 option, 经公共参数透传, 默认 ON; 仅 MSVC 生效)
- 验证 OFF 路径: `cmake -DAGENTXX_MSVC_SPLIT_OBJECTS=OFF <lib-build>` 可正常 configure
  (生成单个 `agentxx_obj.vcxproj`), 再切回 ON 后增量构建稳定 (连跑两次 8.6 s / 4.7 s, 无重编)

### 5. 并行度

- `windows_debug_build.bat` / `windows_release_build.bat`: `--parallel` 默认
  `%NUMBER_OF_PROCESSORS%` (下限 4), 仍可用 `AGENTXX_BUILD_PARALLEL` 覆盖

## 实测结果 (同一台机器: 12 核 / 52 GB, VS18 MSVC 14.51, Debug + ASan, 未装 ccache)

| 场景 | 改前 | 改后 | 倍数 |
| --- | --- | --- | --- |
| 内层 lib no-op | 25.1 s | 8.6 s | 2.9× |
| 内层 test no-op | 18.6 s | 3.8 s | 4.9× |
| lib 全量重建 (107 TU, 静态库+动态库) | 861 s | 194 s | 4.4× |
| lib 改 `context.h` 后重建 (TUs 大面积失效) | 未单测 (按 107 TU × ~10 s 串行估算 ≥ 15 min) | 156 s | ≥ 6× |
| test 全量重建 (152 TU) | 1510 s | 317 s | 4.8× |
| client 全量重建 (36 TU) | 未单测 (串行估算 ~6 min) | 100 s | ~3.6× |
| lib 改 1 个小 TU (静态库+动态库) | 29.6 s | 23.6 s | 1.25× |
| 令顶 no-op | 60.5 s | 67 s | 0.90× (慢 6 s) |
| 令顶改 1 个 lib TU (整条链: lib → install → test/client 重链) | ~108 s (按 no-op + 同样的 4 次链接估算) | 113.7 s | 持平 |

- 令顶 no-op 略变慢是拆工程后的固定开销: 每个 `.vcxproj` 都要被 MSBuild 判一次是否
  过期 (lib +14、test +20、client +5 个工程)。与大范围重建的收益相比这个取舍是划算的;
  觉得单文件迭代慢可以用 `-DAGENTXX_MSVC_SPLIT_OBJECTS=OFF` 回到单工程
- 单文件增量与"改一个 lib 文件 → 整条链重建"基本不变: 时间已被 ASan 下的四次完整链接
  (lib 静态库、lib 动态库、agentxx_test、agentxx) 主导, 与编译并行度无关

## 验证清单

- `agentxx_test event_bridge view_replica plugin_cleanup boundaries`: 464 条全绿
- `event_bridge` 223 条 (含本任务前一步新增的 4 个回归流程)
- `dumpbin /exports libagentxxd.dll`: 35 个 `agentxx_ffi_*`, 与 `ffi_symbols.map` 一致
- `agentxx.exe --version` → `Agentxx v2026.1011.0` (client 拆分后正常)
- `plugin_cleanup` 的"非插件动态库缺入口符号"子项仍能命中 (`libagentxxd.dll` 存在)
- `session_persistence` 仍有 1 个既有失败 (`test_session_persistence.cpp:1341` 非法 UTF-8
  修复断言, 见 `resource/history/async-result-offload-and-resume/work.md`), 与本任务无关

## 可继续优化 (本轮未做)

- **插件资源复制**: lib 每次构建都要跑 29 次 `POST_BUILD`/`custom target` 的
  `copy_if_different` (PerformanceSummary 实测 PostBuildEvent 8.4 s + Exec 8.5 s)。
  改成 `add_custom_command(OUTPUT ...)` 让 MSBuild 做过期判断可省掉, 但要改 25 个插件
  CMakeLists (建议先在 `plugins/cmake/` 加一个 helper 函数再逐个替换)
- **lib install 全部重新 stat 一遍** (数百个 `Up-to-date` 行, 冷启动 9-13 s):
  test/client 目前从 install 树取库, 若改成 `export(EXPORT ...)` 从构建树取,
  dev 环可以跳过 install 步骤 (会影响包导出结构, 需单独评估)
- **换 Ninja 生成器**: Ninja 天然按文件并行且不用 FileTracker, 增量判断更快;
  但脚本目前靠 VS 生成器的 `-A x64` 自动发现工具链, 换生成器要先 `VsDevCmd`
- **Linux/macOS**: 本轮改动在那边全部走"不拆分"路径 (行为不变), 仍建议装 ccache
  (脚本已支持 `CMAKE_*_COMPILER_LAUNCHER`, PCH 的 `-fpch-preprocess` 也已配好)
