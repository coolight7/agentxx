# cxx_utilxx

重依赖工具库 (网络 / 存储 / 正则 / 进程 / 散列)。

## 定位

- **用途**: HTTP 客户端与服务端 (Boost.Beast + OpenSSL)、WebSocket 客户端、SQLite 封装、
  全局设置库 (KV)、正则 (HyperScan 或 std::regex)、Aho-Corasick、路由、文本差异、
  git worktree、MD5/设备标识
- **依赖**: `cxx_utilxx_base` (基础件与取消抽象)、Boost (beast/process/asio)、OpenSSL、
  SQLite3、fmt、html2md; 可选 HyperScan (正则加速)、io_uring (Linux 文件异步 I/O)
- **命名空间**: `utilxx` (与 `cxx_utilxx_base` 的 `utilxx/cancel.h`、`utilxx/async_offload.h` 同一命名空间)
- **禁止**依赖 neograph / agentxx 头文件

## 目录结构

```
include/utilxx/
  http_client.h  http_header.h  http_error.h  http_server.h  ws_client.h
  router.h  sqlite.h  settings_db.h  regex.h  aho_corasick.h
  diff_util.h  worktree.h  crypto.h
src/                    实现 (crypto/http_client/http_header/http_server/regex/
                             settings_db/sqlite/ws_client)
```

## 构建与使用

```cmake
find_package(cxx_utilxx REQUIRED)
target_link_libraries(your_target PRIVATE cxx_utilxx_static)  # 或 cxx_utilxx_shared
```

- 产物命名 (同 libagentxx): Release `libcxx_utilxx.so` / `libcxx_utilxx_static.a`,
  Debug 追加 `d` → `libcxx_utilxxd.so` / `libcxx_utilxx_staticd.a`
- 可选特性由 CMake 开关控制: `CXX_UTILXX_ENABLE_HYPERSCAN` (正则实现)、
  `CXX_UTILXX_ENABLE_BOOST_PROCESS` (worktree 子进程调用)、
  `CXX_UTILXX_LINUX_IO_URING_SUPPORTED` (asio 文件异步 I/O)
- 条件依赖 (hyper scan / io_uring) 以**库文件绝对路径**写入导出接口, 处理方无需
  `pkg_check_modules`, 也不依赖宿主定义 `PkgConfig::*` 目标

```c++
#include "utilxx/http_client.h"

auto resp = co_await utilxx::HttpClient::postAsync(url, body, "application/json", headers,
                                                   utilxx::HttpClient::RequestConfig{});
auto json = resp.bodyJson();          // std::optional<utilxx_base::Json>
```

## 子进程相关

`utilxx/worktree.h` 的 git 操作与 HTTP 客户端的 DNS 解析在阻塞路径上工作,
调用方应经 `utilxx::offloadAsync` / `offloadCancellableAsync` 卸载到线程池,
避免阻塞 io_context 线程; 取消语义统一经 `utilxx::CancelToken` (见 `cxx_utilxx_base`)。

## 平台

Linux / Windows / macOS / Android / iOS 均可编译; HyperScan 与 io_uring 仅在
Linux (HyperScan 另支持 Windows 受限支持) 生效, 未启用时自动回退 std::regex /
同步文件 I/O。
