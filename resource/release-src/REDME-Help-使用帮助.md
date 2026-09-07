# Agentxx 使用帮助

## 修改配置
- 首先需要修改 `agentxx-config.yaml` 中的模型配置，预设了几种常用的模型，可以仿照抄写填想用的模型
- 然后需要在 `.env` 文件中填入你的 apikey 即可

## 运行命令行TUI

### windows 系统上使用
- 可以直接双击 `agentxx_cli.exe` 就会启动 TUI 界面
- 也可以先启动命令行 cmd 或 powershell 都行, 然后执行:
```sh
cd {agentxx_cli.exe 所在目录}
.\agentxx_cli.exe
```

### linux 系统上使用
- 类似的直接命令行启动即可
```sh
cd {agentxx_cli 所在目录}
./agentxx_cli
```

## 其他
- 更多参数和使用方法详见 [文档](https://github.com/coolight7/agentxx)
- `agentxx_cli` 不需要依赖第三方库，可以单独复制这个文件和系统动态库到其他目录，然后同样方法启动，但是需要指定 `agentxx-config.yaml` 和 `.env` 文件位置，如果需要插件则应当复制 `plugins` 文件夹内的插件
- 其他编程语言通过 ffi 调用的话只需要引用 `libagentxx.so` 或是 `libagentxx.dll` 动态库即可，不需要其他依赖库