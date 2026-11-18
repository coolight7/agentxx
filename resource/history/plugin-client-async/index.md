# client 插件接口协程化

## 目标

内置 client 插件的注册事务与回调体改成协程/提交即返回，让 client 侧不再依赖
`allowOnPluginThread`（同步桥保留给 JS 引擎这类同步 API 边界）。

## 方案与分步

见 [plan.md](plan.md)，进度见 [work.md](work.md)。

## 状态

进行中（2026-11-18）。

## 结论

待补。
