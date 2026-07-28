#include "agentxx/middlewares/permission.h"

#include "agentxx/middlewares/event_stream.h"
#include "agentxx/middlewares/events.h"

namespace agentxx {
namespace middleware {

PermissionMiddlewareHandle::PermissionMiddlewareHandle(
    std::weak_ptr<agentxx::agent::AgentContext> in_agentContext
) :
    BaseMiddlewareHandle<PermissionMiddlewareState>("PermissionMiddlewareHandle", in_agentContext) {
}

void PermissionMiddlewareHandle::setFilesystemPermission(
    std::string_view   path,
    PermissionOperator op,
    size_t             index
) {
    assert(index == 0 || index == 1);
    filesystemPermission
        .add(path, static_cast<int>(index), std::make_shared<PermissionOperator>(op));
}

asio::awaitable<bool> PermissionMiddlewareHandle::defOnFilesystemHandle(
    const neograph::Tool& item,
    neograph::json&       args,
    size_t                index
) {
    auto        path = args.value<std::string>("path", "");
    std::string re_path;
    auto        handle = filesystemPermission.get(path, static_cast<int>(index), re_path);
    if (nullptr != handle) {
        auto permission = *handle;
        switch (permission) {
            case PermissionOperator::ALLOW:
                co_return true;
            case PermissionOperator::DENY:
                co_return false;
            case PermissionOperator::INTERRUPT:
                // 经总线询问外部授权者 (CLI/GUI/ACP 各注册自己的 prompter)
                // - 无 prompter 注册时 request 返回 nullopt, 默认拒绝以保安全
                co_return co_await requestPermission(
                    item,
                    args,
                    index == FilesystemPermissionREAD ? "filesystem_read" : "filesystem_write",
                    path
                );
        }
    }
    co_return true;
}

asio::awaitable<bool> PermissionMiddlewareHandle::requestPermission(
    const neograph::Tool& item,
    neograph::json&       args,
    std::string           category,
    std::string           target
) {
    auto ctxPtr = agentContext.lock();
    if (!ctxPtr) {
        co_return false;
    }
    auto threadId = args.value("thread_id", std::string{});
    auto session  = ctxPtr->sessions->get(threadId);
    auto bus      = session ? session->bus : nullptr;
    if (!bus) {
        // 无会话总线, 默认拒绝以保安全
        co_return false;
    }
    auto resp = co_await bus->request<events::ReqPermission, events::RespPermission>(
        events::Topic::Permission,
        events::ReqPermission{
            .agentName     = ctxPtr->agentConfig ? ctxPtr->agentConfig->agentName : std::string{},
            .threadId      = std::move(threadId),
            .toolName      = item.get_name(),
            .category      = std::move(category),
            .target        = std::move(target),
            .argumentsJson = args.dump(),
        }
    );
    if (!resp.has_value()) {
        co_return false; // 无 prompter, 拒绝
    }
    co_return resp->decision == events::RespPermission::Decision::Allow;
}

void PermissionMiddlewareHandle::registerFilesystemHandles() {
    auto readHandle
        = [this](const neograph::Tool& item, neograph::json& args) -> asio::awaitable<bool> {
        co_return co_await defOnFilesystemHandle(item, args, FilesystemPermissionREAD);
    };

    handles["filesystem_list"]             = readHandle;
    handles["filesystem_read_text_file"]   = readHandle;
    handles["filesystem_read_binary_file"] = readHandle;
    handles["filesystem_write_file"]
        = [this](const neograph::Tool& item, neograph::json& args) -> asio::awaitable<bool> {
        co_return co_await defOnFilesystemHandle(item, args, FilesystemPermissionWRITE);
    };
    handles["filesystem_edit_text_file"]
        = [this](const neograph::Tool& item, neograph::json& args) -> asio::awaitable<bool> {
        co_return co_await defOnFilesystemHandle(item, args, FilesystemPermissionREAD)
            && co_await defOnFilesystemHandle(item, args, FilesystemPermissionWRITE);
    };
    // handles["filesystem_glob"] = readHandle;
    // handles["filesystem_grep"] = readHandle;
}

void PermissionMiddlewareHandle::registerHandles() {
    registerFilesystemHandles();
}

} // namespace middleware
} // namespace agentxx
