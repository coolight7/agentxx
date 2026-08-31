#include "agentxx/middlewares/permission.h"

#include "agentxx/event/event_stream.h"
#include "agentxx/event/events.h"
#include "agentxx/util/string_util.h"
#include <cctype>

namespace agentxx {
namespace middleware {

namespace {

class DummyPermissionTool : public neograph::Tool {
public:

    explicit DummyPermissionTool(std::string name) : name_(std::move(name)) {}

    neograph::ChatTool get_definition() const override {
        return neograph::ChatTool{
            .name        = name_,
            .description = "",
            .parameters  = neograph::json::object(),
        };
    }

    std::string get_name() const override {
        return name_;
    }

    std::string execute(const neograph::json&) override {
        return {};
    }

private:

    std::string name_;
};

/// 判断规范化路径 (尾斜杠目录前缀) 是否位于指定目录子树内
inline bool isUnderDir(std::string_view dirWithTrailingSlash, std::string_view path) {
    if (dirWithTrailingSlash.empty() || path.empty()) {
        return false;
    }
    if (path == dirWithTrailingSlash) {
        return true; // 目录自身
    }
    // 去掉 path 尾斜杠后做前缀比较 (normalizePermissionPath 输出带尾斜杠)
    if (!path.empty() && path.back() == '/') {
        path.remove_suffix(1);
    }
    return path.size() >= dirWithTrailingSlash.size()
           && path.compare(0, dirWithTrailingSlash.size(), dirWithTrailingSlash) == 0;
}

} // namespace

/// 权限路径规范化: 绝对路径 + Unix 分隔符 + 目录尾斜杠
/// - Windows 文件系统大小写不敏感, 统一转小写使注册规则 (来自配置/工作目录)
///   与请求路径 (模型可能传任意大小写, 如 `d:/...` 或 `D:\...`) 稳定匹配;
///   XXRouter 的树节点按字符串精确查找 (区分大小写), 不统一大小写会漏匹配,
///   表现为 mode: ask 时工作目录内的读写仍被询问
std::string PermissionMiddlewareHandle::normalizePermissionPath(std::string_view path) const {
    return normalizePermissionPath(path, {});
}

std::string PermissionMiddlewareHandle::normalizePermissionPath(
    std::string_view path,
    std::string_view sessionId
) const {
    // 相对路径解析基准: 会话生效工作目录 (worktree 绑定 > 会话工作目录覆写,
    // 回退 AgentConfig::workDir / 进程 cwd), 与 filesystem 工具的解析基准
    // 保持一致, 使注册规则与工具实际访问路径稳定匹配
    std::string baseDir;
    if (auto ctx = agentContext.lock()) {
        baseDir = ctx->getSessionWorkDir(sessionId);
    }
    std::string s = agentxx::util::toUnixStandardDirPath(
        baseDir.empty() ? agentxx::util::toCurrentSystemAbsolutePath(path)
                        : agentxx::util::toCurrentSystemAbsolutePath(path, baseDir)
    );
#if XX_IS_WIN_D
    agentxx::util::toLowerSelf(s);
#endif
    return s;
}

void PermissionMiddlewareHandle::setSessionIsolation(
    std::string_view   sessionId,
    SessionFsIsolation isolation
) {
    sessionIsolations_.insert_or_assign(std::string{sessionId}, std::move(isolation));
}

void PermissionMiddlewareHandle::clearSessionIsolation(std::string_view sessionId) {
    sessionIsolations_.erase(std::string(sessionId));
}

const SessionFsIsolation* PermissionMiddlewareHandle::sessionIsolation(std::string_view sessionId
) const {
    auto it = sessionIsolations_.find(std::string(sessionId));
    return it == sessionIsolations_.end() ? nullptr : &it->second;
}

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
    filesystemPermission.add(
        normalizePermissionPath(path),
        static_cast<int>(index),
        std::make_shared<PermissionOperator>(op)
    );
}

asio::awaitable<bool> PermissionMiddlewareHandle::defOnFilesystemHandle(
    const neograph::Tool& item,
    neograph::json&       args,
    size_t                index
) {
    auto path      = args.value<std::string>("path", "");
    auto sessionId = args.value("sessionId", std::string{});
    // 支持相对路径: 非绝对路径基于会话生效工作目录 (worktree 绑定优先, 回退
    // AgentConfig::workDir / 进程 cwd) 拼接为绝对路径, 与 filesystem 工具实际
    // 访问的路径保持一致, 使注册的绝对路径规则也能匹配相对路径访问
    path = normalizePermissionPath(path, sessionId);
    if (path.empty()) {
        co_return true;
    }
    // worktree 会话隔离边界 (优先于一切已注册规则):
    // - 绑定 worktree 的会话对主检出子树的写操作直接拒绝 (读不受限),
    //   保证多会话并行开发互不干扰; 拒绝以 tool 结果形式反馈给模型
    if (index == FilesystemPermissionWRITE) {
        if (auto iso = sessionIsolation(sessionId); iso && !iso->denyWritePath.empty()) {
            if (isUnderDir(iso->denyWritePath, path)) {
                XX_LOGD(
                    "Permission: session '{}' isolated by worktree, deny write to main checkout: {}",
                    sessionId,
                    path
                );
                co_return false;
            }
        }
    }
    std::string re_path;
    // 最长前缀匹配: 注册的文件夹规则 (如 /data/projects) 对其下任意子路径生效
    auto handle = filesystemPermission.get(path, static_cast<int>(index), re_path, true);
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
    // 未命中任何规则: 按 noRuleOperator 处理 (CodeAgent 按 permission.mode 设置;
    // 默认 ALLOW 与历史行为一致, 无规则即放行)
    switch (noRuleOperator) {
        case PermissionOperator::ALLOW:
            co_return true;
        case PermissionOperator::DENY:
            co_return false;
        case PermissionOperator::INTERRUPT:
            co_return co_await requestPermission(
                item,
                args,
                index == FilesystemPermissionREAD ? "filesystem_read" : "filesystem_write",
                path
            );
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
    auto sessionId = args.value("sessionId", std::string{});
    auto session   = ctxPtr->sessions->get(sessionId);
    auto bus       = session ? session->bus : nullptr;
    if (!bus) {
        // 无会话总线, 默认拒绝以保安全
        co_return false;
    }
    // 不限制等待时间: 用户可能长时间未响应权限询问,
    // 避免被总线默认 30s 超时截断导致权限被误判为拒绝
    auto resp = co_await bus->request<events::ReqPermission, events::RespPermission>(
        events::Topic::Permission,
        events::ReqPermission{
            .agentName     = ctxPtr->agentConfig ? ctxPtr->agentConfig->agentName : std::string{},
            .sessionId     = std::move(sessionId),
            .toolName      = item.get_name(),
            .category      = std::move(category),
            .target        = std::move(target),
            .argumentsJson = args.dump(),
        },
        std::chrono::milliseconds{0} // 0 = 不限制
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

    handles["agentxx_filesystem_list"] = readHandle;
    handles["agentxx_filesystem_read"] = readHandle;
    handles["agentxx_filesystem_write"]
        = [this](const neograph::Tool& item, neograph::json& args) -> asio::awaitable<bool> {
        co_return co_await defOnFilesystemHandle(item, args, FilesystemPermissionWRITE);
    };
    handles["agentxx_filesystem_edit"]
        = [this](const neograph::Tool& item, neograph::json& args) -> asio::awaitable<bool> {
        co_return co_await defOnFilesystemHandle(item, args, FilesystemPermissionWRITE);
    };
    // handles["agentxx_filesystem_glob"] = readHandle;
    // handles["agentxx_filesystem_grep"] = readHandle;
}

void PermissionMiddlewareHandle::registerHandles() {
    registerFilesystemHandles();
}

PermissionMiddlewareHandle::~PermissionMiddlewareHandle() {
    unregisterFromBus();
}

void PermissionMiddlewareHandle::registerOnBus(const std::shared_ptr<agentxx::event::EventBus>& bus) {
    if (!bus) {
        return;
    }
    unregisterFromBus();
    registeredBus_ = bus;

    // 1. 注册权限检查服务端 (ReqToolPermissionCheck -> RespToolPermissionCheck)
    checkServerId_ = bus->getRR<events::ReqToolPermissionCheck, events::RespToolPermissionCheck>(
        events::Topic::ToolPermissionCheck
    ).registerServer([this](const events::ReqToolPermissionCheck& req, size_t) -> asio::awaitable<events::RespToolPermissionCheck> {
        auto it = handles.find(req.toolName);
        if (it != handles.end()) {
            DummyPermissionTool dummyTool(req.toolName);
            neograph::json argsCopy = req.arguments;
            auto allow = co_await it->second(dummyTool, argsCopy);
            co_return events::RespToolPermissionCheck{.allow = allow};
        }
        // 未注册权限拦截 handle 的普通工具直接放行
        co_return events::RespToolPermissionCheck{.allow = true};
    });

    // 2. 订阅文件系统规则设置事件 (EventSetPermissionRule)
    setRuleSubId_ = bus->get<events::EventSetPermissionRule>(events::Topic::PermissionSetRule)
        .subscribe([this](const events::EventSetPermissionRule& evt) -> asio::awaitable<void> {
            setFilesystemPermission(
                evt.path,
                evt.allow ? PermissionOperator::ALLOW : PermissionOperator::DENY,
                evt.index
            );
            co_return;
        });

    // 3. 订阅会话隔离设置事件 (EventSetSessionIsolation)
    setIsolationSubId_ = bus->get<events::EventSetSessionIsolation>(events::Topic::PermissionSetIsolation)
        .subscribe([this](const events::EventSetSessionIsolation& evt) -> asio::awaitable<void> {
            SessionFsIsolation iso;
            iso.allowPath     = normalizePermissionPath(evt.allowPath);
            iso.denyWritePath = normalizePermissionPath(evt.denyWritePath);
            setSessionIsolation(evt.sessionId, std::move(iso));
            co_return;
        });

    // 4. 订阅会话隔离清除事件 (EventClearSessionIsolation)
    clearIsolationSubId_ = bus->get<events::EventClearSessionIsolation>(events::Topic::PermissionClearIsolation)
        .subscribe([this](const events::EventClearSessionIsolation& evt) -> asio::awaitable<void> {
            clearSessionIsolation(evt.sessionId);
            co_return;
        });
}

void PermissionMiddlewareHandle::unregisterFromBus() {
    if (auto bus = registeredBus_.lock()) {
        if (checkServerId_ != 0) {
            bus->getRR<events::ReqToolPermissionCheck, events::RespToolPermissionCheck>(
                events::Topic::ToolPermissionCheck
            ).unregisterServer(checkServerId_);
            checkServerId_ = 0;
        }
        if (setRuleSubId_ != 0) {
            bus->get<events::EventSetPermissionRule>(events::Topic::PermissionSetRule)
                .unsubscribe(setRuleSubId_);
            setRuleSubId_ = 0;
        }
        if (setIsolationSubId_ != 0) {
            bus->get<events::EventSetSessionIsolation>(events::Topic::PermissionSetIsolation)
                .unsubscribe(setIsolationSubId_);
            setIsolationSubId_ = 0;
        }
        if (clearIsolationSubId_ != 0) {
            bus->get<events::EventClearSessionIsolation>(events::Topic::PermissionClearIsolation)
                .unsubscribe(clearIsolationSubId_);
            clearIsolationSubId_ = 0;
        }
    }
    registeredBus_.reset();
}

} // namespace middleware
} // namespace agentxx
