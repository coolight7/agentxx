# 修复 android release 交叉编译失败 (静态 OpenSSL 链入共享库的 aarch64 重定位报错)
- 难度: C
- 类型: bug修复
- 基于commit: c46cebe21b686902c60a4736df6f7a4d94111788
- 时间: 2026-10-12 08:50
- 需求:
```md
请执行 ./agent/script/cross_android_release_build.sh 编译安卓release
```

## 现象与根因

`cross_android_release_build.sh` (arm64-v8a) 在链接 `libcxx_utilxx.so` 时失败:

```
ld.lld: error: relocation R_AARCH64_ADR_PREL_PG_HI21 cannot be used against symbol 'poly1305_blocks_sve2'; recompile with -fPIC
ld.lld: error: relocation R_AARCH64_ADD_ABS_LO12_NC cannot be used against symbol 'poly1305_blocks_sve2'; recompile with -fPIC
>>> referenced by libcrypto-lib-poly1305-armv8.o:(poly1305_init) in archive .../OpenSSL-android-build/arm64-v8a/lib/libcrypto.a
```

根因在 OpenSSL 4.0.1 的 aarch64 汇编 (与我们的代码无关):

- `crypto/poly1305/asm/poly1305-armv8.pl` 生成的 `poly1305_init` 用 `adrp/add`
  直接取 `poly1305_blocks_sve2` (按 `ARMV9_SVE2_POLY1305` 运行时能力选函数指针),
  而该符号定义在另一个汇编文件 `poly1305-armv9-sve2.pl` 生成的目标文件里,
  引用处只有 `.extern`、**没有 `.hidden`**。
- 把静态 `libcrypto.a` 链进共享库时, 这个符号在输出里仍是可抢占的全局符号
  (默认可见性且进入动态符号表), 直接重定位在这种符号上需要走 GOT,
  链接器于是直接拒绝 (报 "recompile with -fPIC", 实际汇编已经是 PIC)。
- OpenSSL 自己构建 `libcrypto.so` 时靠版本脚本 (`util/libcrypto.num` 未收录该符号)
  把它变成局部符号, 因此上游构建没有暴露这个问题; 上游 master 至今也只写 `.extern`。
- 只有 aarch64 会碰到: x86_64 的对应汇编没有这种跨目标文件直接取地址的写法,
  所以 Linux x86_64 一直链接正常。

## 修改

- `agent/CMakeLists.txt` (非 MSVC 分支内, 仅 Android): 给共享库链接参数追加
  `-Wl,--exclude-libs=ALL` —— 链接器把来自静态库的符号按隐藏可见性处理
  (不进入动态符号表), 重定位不再需要 GOT; 顺带不再把 OpenSSL/fmt 等第三方符号
  导出到我们自己的 .so。
  - 只加给 SHARED (可执行文件不导出动态符号, 加不加都一样)
  - 本项目自己的 C/C++ API 由源文件直接编入共享库 (OBJECT 库), 不经过静态库,
    导出面不受影响
  - 该变量经 `_AGENTXX_COMMON_CMAKE_ARGS` 下发, 覆盖全部嵌套 (ExternalProject) 构建
- `agent/script/cross_android_release_build.sh`:
  - 补上与 linux/macos 构建脚本一致的 ccache 配置段 (`CCACHE_DIR` 默认
    `~/.cache/ccache-agentxx`, `CCACHE_MAXSIZE` 默认 20G, `CCACHE_BASEDIR` 归一化路径)。
    首次尝试时用的是系统 ccache 默认缓存 (上限 5 G, 已用 88%), 在容量清理过程中
    ccache 自身 SIGABRT (内核日志 "ccache: potentially unexpected fatal signal 6"),
    换了独立缓存目录后构建正常。
  - exec 目录除 `*.so` 外, 对可执行文件 (存在才处理) 补 `llvm-strip --strip-all`:
    NDK 工具链默认给所有配置加 `-g`, 原来 324 MB 的 `agentxx_cli` strip 后 24.8 MB
    (与 `script/linux_release_build.sh` 对 `agentxx_cli`/`agentxx_benchmark` 的处理一致)
- `docs/zh-cn/build/android.md`: 补"常见错误"条目 (链接报 poly1305_blocks_sve2 重定位)
  与编译结果说明 (产物已 strip)。
- `AGENTS.md`: 常见问题补一条同样的记录, 便于后续排查。

## 验证

- `AGENTXX_BUILD_PARALLEL=8 ./agent/script/cross_android_release_build.sh` 全流程通过
  (cmake configure + build + install + 复制 `libc++_shared.so` + strip), 日志无报错;
  再跑一次确认可重复执行 (幂等, 全部 up-to-date)。
- 产物检查 (NDK r29 `llvm-readelf`):
  - `exec/libagentxx.so` 17 MB, 定义导出的全局符号正好 35 个, 全是 `agentxx_ffi_*`
    (ffi_symbols.map 白名单未被破坏)
  - `exec/libcxx_utilxx.so` 12 MB, 导出 89 个自身 `utilxx::` API, 0 个
    OpenSSL/fmt/simdjson 符号
  - 全部产物 (含 17 个插件 .so) 动态符号表里都没有 `poly1305*`;
    DT_NEEDED 只有 `libc++_shared.so`/`libc.so`/`libm.so`/`libdl.so`;
    插件仍只导出 5 个 `agentxx_plugin_*` 入口
  - `exec/agentxx_cli` 已 stripped, 24.8 MB, DT_NEEDED 同上
- 最小复现实验: 把 `libcrypto.a` 的 `poly1305-armv8.o` + `poly1305-armv9-sve2.o`
  用 `--whole-archive` 链成 .so, 不加选项时复现同样的 lld 重定位错误,
  加上 `--exclude-libs=ALL` 后链接通过 (纯对象文件直连时该选项无效, 只对静态库成员生效)。
