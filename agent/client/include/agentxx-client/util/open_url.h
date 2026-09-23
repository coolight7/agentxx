#pragma once
#include <string_view>

namespace agentxx::client {

/// 判断链接是否可以交给系统默认程序打开
///
/// 只接受 `http://` / `https://` 开头且协议后有内容的链接, 且链接中的 ASCII
/// 字符必须落在 URI 允许字符集 (RFC 3986, 额外去掉单引号) 内: 双引号/反引号/
/// 反斜杠/单引号/空格/控制字符/花括号/竖线等会让交给 shell 的命令串被截断或
/// 注入的字符一律拒绝; 非 ASCII 字节按 UTF-8 直接放行。
/// 其它协议 (file: / javascript: 等) 一律拒绝: 界面上只用于打开发布页这类网页链接。
///
/// - `url`: 待校验的 UTF-8 链接
/// - `return`: 可以打开返回 true
bool isOpenableUrl(std::string_view url);

/// 用系统默认程序打开链接 (通常落到浏览器)
///
/// - Windows: `ShellExecuteW("open")`; macOS: `open`; 其它 (Linux/Android 等): `xdg-open`
/// - 实际打开动作在后台线程执行 (命令行工具可能因桌面环境无响应而阻塞, 不阻塞
///   调用线程); 子进程输出被重定向丢弃, 不会污染 TUI 画面
/// - 只在链接校验通过时发起: 校验不通过 (协议不支持/含非法字符) 不执行任何命令
///
/// - `url`: 待打开的 UTF-8 链接
/// - `return`: false = 链接未通过校验, 未发起打开; true = 已发起打开请求
///   (系统是否真的打开成功无法在此确认)
bool openUrlInBrowser(std::string_view url);

} // namespace agentxx::client
