#pragma once

// 控制台状态守护 (Windows)
//
// 背景: TUI 依赖"控制台输出代码页 = UTF-8"与"控制台启用了 VT 处理/输入",
// 而这两项属于**控制台** (conhost / Windows Terminal) 的共享状态, 不是进程私有:
// agentxx_execute_command 等工具的子进程默认继承 agent 进程的控制台 (管道只重定向
// std 句柄), 其中任何调用 SetConsoleOutputCP 的程序 (`chcp`、批处理脚本、部分
// 原生工具) 都会改掉它, 子进程退出后也不会恢复。
//
// 一旦被改掉, TUI 会出现两类症状 (同一次改动引起, 常同时出现):
// 1. 中文整屏乱码: TUI 写的是 UTF-8 字节, 终端却按新代码页解码。
// 2. 画面整体漂移、底部不断留下空白 (看起来像"最新消息间距突然变得很大"):
//    列宽假设失效 —— 一个中文字在 UTF-8 里是 3 字节, 按 936 解码约占 3 列,
//    而 FTXUI 按 2 列排版, 于是行宽溢出触发终端自动换行; FTXUI 每帧靠
//    "光标上移 dimy-1 行"的相对定位回到帧首, 折行数不对就会落在错误的行,
//    画面逐帧错位并累积空白, 直到重启进程 (重启后重新写入全屏帧才对齐)。
//
// 因此 TUI 每帧开头调用一次 [ConsoleStateGuard::check] 做自检 (代价是一次
// GetConsoleOutputCP): 代码页/模式被外部改过就改回来, 并让调用方清屏 + 回原点,
// 使画面重新对齐。
namespace agentxx::client {

/// 控制台状态自检结果 (见 [ConsoleStateGuard::check])
struct ConsoleStateFix {
    /// 检出并尝试修复了被外部改动的状态
    bool repaired = false;
    /// 状态已成功写回; 调用方应随即写出 [consoleRealignSequence] 让画面重新对齐
    bool needResync = false;
    /// 修复前的输出代码页 (代码页被改动时有效; 诊断日志用)
    unsigned int previousOutputCodePage = 0;
    /// 本次是否改写了控制台模式
    bool modeRepaired = false;
};

/// 控制台状态守护
///
/// 保存"重试已放弃"的标记: 终端不支持某个必需位时, 写回会一直失败 ——
/// 若每帧仍判定为缺失, 调用方就会每帧清屏重绘 (画面持续闪烁), 所以失败后停止重试。
/// 一个进程用一个实例即可 (TUI 放在 UI 线程独占的成员里, CLI 放在 IO 对象里)。
class ConsoleStateGuard {
public:
    /// 检查并恢复 TUI 依赖的控制台状态
    ///
    /// 检查项:
    /// - 输出/输入代码页: 都必须是 UTF-8 (CP_UTF8)
    /// - 输出模式: 必须含 `ENABLE_VIRTUAL_TERMINAL_PROCESSING`
    ///   (缺了终端就不解释 ANSI 转义序列, 画面必乱)
    /// - 输入模式: 必须含 `ENABLE_VIRTUAL_TERMINAL_INPUT` + `ENABLE_WINDOW_INPUT`
    ///   (FTXUI 靠它拿到按键与窗口事件; 缺了表现为界面不响应按键)
    ///
    /// 不检查 `ENABLE_LINE_INPUT` / `ENABLE_ECHO_INPUT` / `ENABLE_WRAP_AT_EOL_OUTPUT`:
    /// 实测 conhost 在开启 VT 输入后仍保持前两位为 1, 并自行维护 WRAP 位;
    /// 把它们当判据会每帧误判成"被改动", 于是每帧清屏 (画面持续闪烁)。
    ///
    /// - 未附加控制台 (服务/管道运行, `GetConsoleOutputCP() == 0`) 时不做任何事
    /// - std 句柄被重定向到管道/文件时 (`GetConsoleMode` 失败) 不碰它的模式,
    ///   但代码页照常检查 (同一个控制台的其它程序仍受它影响)
    ///
    /// - `return` 被改动过时 repaired = true; 成功写回时 needResync = true,
    ///   调用方随后须写出 [consoleRealignSequence] 清屏回原点
    ConsoleStateFix check();

    /// 是否已放弃修复 (终端拒绝写回模式位)
    bool gaveUp() const {
        return outGiveUp_ || inGiveUp_;
    }

private:
    /// 输出模式写回失败 (终端不支持 VT 处理): 不再重试
    bool outGiveUp_ = false;
    /// 输入模式写回失败 (终端不支持 VT 输入): 不再重试
    bool inGiveUp_ = false;
};

/// 修复控制台状态后必须先写给终端的内容: 复位字符属性 + 清屏 + 光标回原点
///
/// 为什么必须清屏: FTXUI 的帧首定位是**相对**的 (`\r` + 光标上移 dimy-1 行),
/// 漂移发生后无法靠它自己回到正确的行; 清屏 + home 把物理画面和光标都归一,
/// 之后 FTXUI 的相对定位从行首起算, 画面重新对齐。
/// (TUI 使用 FTXUI 的全屏备用屏幕, 帧原点就是屏幕原点)
///
/// - `return` 非 Windows 平台返回空串 (无需重新对齐)
const char* consoleRealignSequence();

} // namespace agentxx::client
