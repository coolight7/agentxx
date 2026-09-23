#include "agentxx-client/util/open_url.h"
#include "utilxx_base/log.h"
#include "utilxx_base/string_util.h"
#include <cstddef>
#include <cstdlib>
#include <string>
#include <thread>

#if XX_IS_WIN_D
#include <windows.h>
// ShellExecuteW 声明在 shellapi.h: 项目其它头文件定义了 WIN32_LEAN_AND_MEAN,
// 该头不会被 windows.h 自动包含, 必须显式包含
#include <shellapi.h>
// ShellExecuteW 位于 shell32; MSVC/MinGW 的默认链接列表一般已含它, 这里再显式
// 声明一次, 避免精简链接配置下找不到符号
#if defined(_MSC_VER) || defined(__MINGW32__)
#pragma comment(lib, "shell32.lib")
#endif
#endif

namespace agentxx::client {

bool isOpenableUrl(std::string_view url) {
    // 协议: 只允许 http/https (网页链接; 其它协议不交给系统默认程序打开),
    // 且协议之后必须还有内容 (拒绝 "https://" 这类空地址)
    std::size_t prefixLen = 0;
    if (url.size() > 7 && url.compare(0, 7, "http://") == 0) {
        prefixLen = 7;
    } else if (url.size() > 8 && url.compare(0, 8, "https://") == 0) {
        prefixLen = 8;
    } else {
        return false;
    }
    if (url.size() <= prefixLen) {
        return false;
    }
    // 字符白名单 = URI 允许字符 (RFC 3986) 去掉单引号:
    // 交给 shell 的命令串用单引号包裹, 白名单外的字符 (双引号/反引号/反斜杠/
    // 单引号/空格/控制字符/花括号/竖线等) 一律拒绝, 杜绝命令串被截断或注入。
    // 非 ASCII 字节 (UTF-8) 直接放行: 单引号内不参与 shell 解析, Windows 侧经
    // ShellExecuteW 按宽字符处理
    for (const char ch : url) {
        const auto c = static_cast<unsigned char>(ch);
        if (c >= 0x80) {
            continue;
        }
        const bool alnum = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z')
                           || (c >= 'A' && c <= 'Z');
        if (alnum) {
            continue;
        }
        switch (ch) {
            case '-':
            case '.':
            case '_':
            case '~':
            case ':':
            case '/':
            case '?':
            case '#':
            case '[':
            case ']':
            case '@':
            case '!':
            case '$':
            case '&':
            case '(':
            case ')':
            case '*':
            case '+':
            case ',':
            case ';':
            case '=':
            case '%':
                continue;
            default:
                return false;
        }
    }
    return true;
}

bool openUrlInBrowser(std::string_view url) {
    if (!isOpenableUrl(url)) {
        XX_LOGW("[client] refuse to open url: {}", url);
        return false;
    }
    std::string target{url};
    // 打开动作放到后台线程: 命令行工具在部分桌面环境下会阻塞, 调用方 (UI 线程)
    // 不能被它卡住; 线程 detached, 失败只记日志 (界面侧另有提示)
    std::thread([target = std::move(target)] {
#if XX_IS_WIN_D
        // 宽字符 API: 链接中的非 ASCII 字符按 UTF-8 → UTF-16 转换
        // (ShellExecuteW 不经 shell 解析, 无参数注入面)
        const std::wstring wide = utilxx_base::utf8ToPath(target).wstring();
        const auto         ret  = reinterpret_cast<intptr_t>(ShellExecuteW(
            nullptr,
            L"open",
            wide.c_str(),
            nullptr,
            nullptr,
            SW_SHOWNORMAL
        ));
        // ShellExecuteW 返回值 <= 32 表示失败 (见 Win32 文档)
        if (ret <= 32) {
            XX_LOGW("[client] ShellExecute failed ({}) for url: {}", ret, target);
        }
#elif XX_IS_MACOS_D
        // 单引号包裹: 白名单已排除单引号, 命令串不会被截断或注入
        const std::string cmd = "open '" + target + "' >/dev/null 2>&1";
        if (std::system(cmd.c_str()) != 0) {
            XX_LOGW("[client] open command failed for url: {}", target);
        }
#elif XX_IS_ANDROID_D
        // Android (Termux 等环境): 优先 termux-open-url (termux-api 提供),
        // 不可用时回退 xdg-open
        const std::string cmd
            = "termux-open-url '" + target + "' >/dev/null 2>&1 || xdg-open '" + target
              + "' >/dev/null 2>&1";
        if (std::system(cmd.c_str()) != 0) {
            XX_LOGW("[client] open url failed on android: {}", target);
        }
#else
        // Linux 等: xdg-open 按桌面默认程序打开 (未安装时返回非 0)
        const std::string cmd = "xdg-open '" + target + "' >/dev/null 2>&1";
        if (std::system(cmd.c_str()) != 0) {
            XX_LOGW("[client] xdg-open failed for url: {}", target);
        }
#endif
    }).detach();
    return true;
}

} // namespace agentxx::client
