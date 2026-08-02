# 开发提示

## 测试网络连接稳定性处理
- 可以利用 `clash` 等 vpn 软件代理，然后在 agent 运行时即可轻松手动切断其网络连接，以便测试自动重连等功能
```sh
# 挂代理到 clash
export http_proxy=http://127.0.0.1:7980
export https_proxy=http://127.0.0.1:7980

# 在当前 shell 启动 agentxx_cli
agentxx_cli tui

# agentxx_cli 的网络会经过 clash，可以在它的`连接`页面控制关闭
```