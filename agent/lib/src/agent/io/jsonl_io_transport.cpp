#include "agentxx/agent/io/jsonl_io_transport.h"

#include "agentxx/agent/io/wire_protocol.h" // serialize / deserialize (同一份报文编码)
#include "agentxx/util/exception.h"
#include "asio/use_awaitable.hpp"
#include "fmt/format.h"
#include "utilxx_base/json.h"
#include "utilxx_base/log.h"

#if XX_IS_WIN_D
#include <fcntl.h>  // _O_BINARY
#include <io.h>     // _setmode
#endif

#include <cstdio>   // stdin/stdout
#include <iostream>
#include <utility>

namespace agentxx {
namespace agent {
namespace io {

namespace {

/// 去掉首尾空白与行尾 `\r` (返回的文本可能为空)
std::string_view trimLine(std::string_view line) {
    auto isBlank = [](char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f';
    };
    size_t begin = 0;
    size_t end   = line.size();
    while (begin < end && isBlank(line[begin])) {
        ++begin;
    }
    while (end > begin && isBlank(line[end - 1])) {
        --end;
    }
    return line.substr(begin, end - begin);
}

/// 取消息的 `type` 标签 (仅用于错误提示; 解析失败返回空串)
std::string typeTagOf(std::string_view text) {
    return agentxx::util::catchError<std::string>(
        [text]() -> std::string {
            auto json = utilxx_base::Json::parse(text);
            if (!json.is_object()) {
                return {};
            }
            return json.value("type", std::string{});
        },
        [](std::string) -> std::string {
            return {};
        }
    );
}

/// 行是否是一个 JSON 对象 (区分"JSON 非法"与"结构不对"两种错误提示)
bool looksLikeJsonObject(std::string_view text) {
    return agentxx::util::catchError<bool>(
        [text]() -> bool {
            return utilxx_base::Json::parse(text).is_object();
        },
        [](std::string) -> bool {
            return false;
        }
    );
}

#if XX_IS_WIN_D
/// stdin/stdout 切到二进制模式: 关闭 CRLF 转换与 Ctrl+Z 解释
/// (JSONL 分帧靠 `\n` 本身, 文本模式的隐式转换会让行内容多出 `\r`)
void setStdioBinaryMode() {
    (void)::_setmode(::_fileno(stdin), _O_BINARY);
    (void)::_setmode(::_fileno(stdout), _O_BINARY);
}
#else
void setStdioBinaryMode() {}
#endif

} // namespace

// ---------------------------------------------------------------------------
// 分帧编解码
// ---------------------------------------------------------------------------

JsonlFrame jsonlDecodeLine(std::string_view line) {
    JsonlFrame frame;
    const auto text = trimLine(line);
    if (text.empty()) {
        return frame; // 空行: 调用方跳过
    }
    if (text.size() > kJsonlMaxLineBytes) {
        frame.error = fmt::format(
            "jsonl line too long ({} bytes > {} limit)",
            text.size(),
            kJsonlMaxLineBytes
        );
        return frame;
    }

    auto parsed = io::deserialize(text);
    if (parsed.has_value()) {
        frame.message = std::move(parsed);
        return frame;
    }

    // 解析失败: 区分"不是合法 JSON"与"类型不认识", 便于对端排查
    if (!looksLikeJsonObject(text)) {
        frame.error = "invalid jsonl line: not a JSON object (expected one Wire message per line)";
        return frame;
    }
    const auto tag = typeTagOf(text);
    if (tag.empty()) {
        frame.error = "invalid jsonl line: missing `type` field";
        return frame;
    }
    frame.error = fmt::format("unsupported wire message type `{}`", tag);
    return frame;
}

std::string jsonlEncodeMessage(const WireMessage& msg) {
    auto text = io::serialize(msg);
    if (text.empty()) {
        // serialize 不会为空 (至少含 type); 兜底避免写出一条空行让对端停摆
        text = R"({"type":"error","message":"serialize produced empty text"})";
    }
    text.push_back('\n');
    return text;
}

} // namespace io

// ---------------------------------------------------------------------------
// JsonlAgentIOTransport
// ---------------------------------------------------------------------------

JsonlAgentIOTransport::JsonlAgentIOTransport(asio::any_io_executor ex, LineIo io) :
    ex_(std::move(ex)), io_(std::move(io)) {
    reader_       = std::make_shared<ReaderState>();
    reader_->lines = std::make_shared<LineChannel>(ex_, 64);
    if (!io_.readLine) {
        // 默认输入为 stdin: 二进制模式 + 后台线程阻塞读行
        io::setStdioBinaryMode();
        startStdioReader();
    }
    if (!io_.writeLine) {
        // 默认输出为 stdout: 逐行写协议报文 (行文本已含换行), 写后 flush
        io::setStdioBinaryMode();
        io_.writeLine = [](std::string_view line) {
            std::cout.write(line.data(), static_cast<std::streamsize>(line.size()));
            std::cout.flush();
        };
    }
    XX_LOGD(
        "[jsonl] transport ready (input={}, output={})",
        io_.readLine ? "injected reader" : "stdin",
        io_.writeLine ? "line writer" : "none"
    );
}

JsonlAgentIOTransport::~JsonlAgentIOTransport() {
    close();
    // 读行线程可能在 std::getline 上阻塞 (对端仍开着管道), 不能在此 join;
    // 线程只持有 reader_ (shared_ptr), 与本对象生命周期无关, 读到下一行或 EOF
    // 后自行退出 (进程退出时由系统回收)
    if (readerThread_.joinable()) {
        readerThread_.detach();
    }
}

void JsonlAgentIOTransport::startStdioReader() {
    auto state = reader_;
    readerThread_ = std::thread([state]() {
        std::string line;
        while (!state->stop.load(std::memory_order_acquire)) {
            if (!std::getline(std::cin, line)) {
                // EOF / 读错误: 用 channel_cancelled 唤醒等待中的 recv
                state->lines->async_send(
                    asio::experimental::channel_errc::channel_cancelled,
                    std::string{},
                    [](utilxx_base::AsioErrorCode) {}
                );
                return;
            }
            state->lines->async_send(
                utilxx_base::AsioErrorCode{},
                std::move(line),
                [](utilxx_base::AsioErrorCode) {}
            );
        }
    });
}

std::optional<std::string> JsonlAgentIOTransport::pollInjectedLine() {
    std::string line;
    if (!io_.readLine || !io_.readLine(line)) {
        return std::nullopt;
    }
    return std::optional<std::string>{std::move(line)};
}

bool JsonlAgentIOTransport::writeLine(std::string_view line) {
    if (!io_.writeLine) {
        return false;
    }
    std::lock_guard<std::mutex> lock(writeMu_);
    io_.writeLine(line);
    linesWritten_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void JsonlAgentIOTransport::reportInvalidLine(std::string_view reason) {
    invalidLines_.fetch_add(1, std::memory_order_relaxed);
    XX_LOGW("[jsonl] rejected input line: {}", reason);
    WireError err;
    err.code    = WireErrorCode::InvalidArgs;
    err.message = std::string{reason};
    writeLine(io::jsonlEncodeMessage(WireMessage{std::move(err)}));
}

void JsonlAgentIOTransport::send(WireMessage msg) {
    if (closed_.load(std::memory_order_acquire)) {
        XX_LOGD("[jsonl] send dropped: transport closed");
        return;
    }
    writeLine(io::jsonlEncodeMessage(msg));
}

asio::awaitable<std::optional<WireMessage>> JsonlAgentIOTransport::recv() {
    for (;;) {
        if (closed_.load(std::memory_order_acquire)) {
            co_return std::nullopt;
        }
        if (inputEnded_.load(std::memory_order_acquire)) {
            co_return std::nullopt;
        }

        std::optional<std::string> line;
        if (io_.readLine) {
            line = pollInjectedLine();
        } else {
            // 等待读行线程投递 (channel 关闭/取消都按"输入结束"处理)
            auto received = co_await agentxx::util::catchErrorAsync<std::optional<std::string>>(
                [&]() -> asio::awaitable<std::optional<std::string>> {
                    auto text = co_await reader_->lines->async_receive(asio::use_awaitable);
                    co_return std::optional<std::string>{std::move(text)};
                },
                [](std::string) -> asio::awaitable<std::optional<std::string>> {
                    co_return std::nullopt;
                }
            );
            line = std::move(received);
        }

        if (!line.has_value()) {
            // 输入结束 (stdin EOF): 标记后让调用方的接收循环退出; 传输仍可发送,
            // 使进行中的轮次能把结果写完 (见头文件"输入结束与关闭分开")
            if (inputEnded_.exchange(true, std::memory_order_acq_rel) == false) {
                XX_LOGI("[jsonl] input ended (stdin closed)");
            }
            co_return std::nullopt;
        }
        linesRead_.fetch_add(1, std::memory_order_relaxed);

        auto frame = io::jsonlDecodeLine(*line);
        if (!frame.error.empty()) {
            reportInvalidLine(frame.error);
            continue; // 非法行不中断会话
        }
        if (!frame.message.has_value()) {
            continue; // 空行
        }
        co_return std::move(frame.message);
    }
}

void JsonlAgentIOTransport::close() {
    if (closed_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    if (reader_) {
        reader_->stop.store(true, std::memory_order_release);
        if (reader_->lines) {
            // 唤醒挂起的 async_receive (取消的接收按"输入结束"处理)
            (void)agentxx::util::catchError<int>(
                [this]() -> int {
                    reader_->lines->cancel();
                    return 0;
                },
                [](std::string) -> int {
                    return -1;
                }
            );
        }
    }
}

bool JsonlAgentIOTransport::alive() const noexcept {
    return !closed_.load(std::memory_order_acquire);
}

void JsonlAgentIOTransport::setStage(WireConnectionStage stage, std::string_view reason) {
    const auto previous = stage_.exchange(stage, std::memory_order_acq_rel);
    if (previous != stage) {
        XX_LOGI(
            "[jsonl] connection stage {} -> {} ({})",
            wireConnectionStageText(previous),
            wireConnectionStageText(stage),
            reason
        );
    }
}

JsonlAgentIOTransport::Stats JsonlAgentIOTransport::stats() const noexcept {
    Stats out;
    out.linesRead    = linesRead_.load(std::memory_order_relaxed);
    out.linesWritten = linesWritten_.load(std::memory_order_relaxed);
    out.invalidLines = invalidLines_.load(std::memory_order_relaxed);
    return out;
}

} // namespace agent
} // namespace agentxx
