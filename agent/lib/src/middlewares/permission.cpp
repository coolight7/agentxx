#include "agentxx/middlewares/permission.h"
#include "agentxx/tools/tool.h"

#include "agentxx/event/event_stream.h"
#include "agentxx/event/events.h"
#include "agentxx/util/string_util.h"
#include <cctype>
#include <filesystem>

namespace agentxx {
namespace middleware {

namespace {

class DummyPermissionTool : public agentxx::tools::XXToolBase {
public:

    explicit DummyPermissionTool(std::string name) :
        agentxx::tools::XXToolBase(name, {}) {}

    neograph::ChatTool get_definition() const override {
        return neograph::ChatTool{
            .name        = name,
            .description = "",
            .parameters  = neograph::json::object(),
        };
    }

    asio::awaitable<std::string> execute_async(const agentxx::util::Json&) override {
        co_return std::string{};
    }
};

/// 判断规范化路径 path 是否位于指定目录 dir 子树内 (或就是目录自身)
inline bool isUnderDir(std::string_view dir, std::string_view path) {
    if (dir.empty() || path.empty()) {
        return false;
    }
    while (dir.size() > 1 && dir.back() == '/') {
        dir.remove_suffix(1);
    }
    while (path.size() > 1 && path.back() == '/') {
        path.remove_suffix(1);
    }
    if (path == dir) {
        return true; // 目录自身
    }
    if (dir == "/") {
        return path.starts_with('/');
    }
    return path.size() > dir.size() && path[dir.size()] == '/'
           && path.compare(0, dir.size(), dir) == 0;
}

} // namespace

/// 权限路径规范化: 绝对路径 + Unix 分隔符 (+ Windows 转小写)
/// - 目录路径 (在文件系统中实际存在且为目录, 或原路径以 '/' 或 '\\' 结尾) 追加/保留尾斜杠
/// - 文件路径 (在文件系统中实际存在且为普通文件, 或实际不存在且原路径未以斜杠结尾) 确保不带尾斜杠,
///   避免对普通文件请求权限时被错误添加末尾 '/'
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
    if (path.empty()) {
        return {};
    }
    // 相对路径解析基准: 会话生效工作目录 (worktree 绑定 > 会话工作目录覆写,
    // 回退 AgentConfig::workDir / 进程 cwd), 与 filesystem 工具的解析基准
    // 保持一致, 使注册规则与工具实际访问路径稳定匹配
    std::string baseDir;
    if (auto ctx = agentContext.lock()) {
        baseDir = ctx->getSessionWorkDir(sessionId);
    }
    std::string absPath = agentxx::util::toCurrentSystemAbsolutePath(path, baseDir);
    std::string s       = agentxx::util::toUnixStandardPath(absPath);

    // 判断是否为目录:
    // 1. 在文件系统上实际存在且为目录
    // 2. 或文件系统上不存在, 但原路径末尾显式带有斜杠 (如 "dir/" 或 "dir\\") 表达目录意图
    // 对于普通文件 (或不存在且原路径无尾斜杠的文件路径), 绝不添加尾部 '/', 若有则去除
    std::error_code ec;
    auto            fsPath = agentxx::util::utf8ToPath(absPath);
    bool            exists = std::filesystem::exists(fsPath, ec);
    bool            isDir  = false;
    if (!ec && exists) {
        isDir = std::filesystem::is_directory(fsPath, ec);
    } else {
        isDir = (path.back() == '/' || path.back() == '\\');
    }

    if (isDir) {
        if (s.empty() || s.back() != '/') {
            s += '/';
        }
    } else {
        while (s.size() > 1 && s.back() == '/') {
            s.pop_back();
        }
    }

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

void PermissionMiddlewareHandle::addConfigDenyPath(std::string_view path) {
    if (path.empty()) {
        return;
    }
    const auto norm = normalizePermissionPath(path);
    if (norm.empty()) {
        return;
    }
    configDenyPermission_.add(
        norm,
        static_cast<int>(FilesystemPermissionREAD),
        std::make_shared<PermissionOperator>(PermissionOperator::DENY)
    );
    configDenyPermission_.add(
        norm,
        static_cast<int>(FilesystemPermissionWRITE),
        std::make_shared<PermissionOperator>(PermissionOperator::DENY)
    );
    // 同时注册到常规 filesystemPermission 规则表 (保持统一)
    filesystemPermission.add(
        norm,
        static_cast<int>(FilesystemPermissionREAD),
        std::make_shared<PermissionOperator>(PermissionOperator::DENY)
    );
    filesystemPermission.add(
        norm,
        static_cast<int>(FilesystemPermissionWRITE),
        std::make_shared<PermissionOperator>(PermissionOperator::DENY)
    );
}

bool PermissionMiddlewareHandle::isConfigDenied(std::string_view path, size_t index) const {
    if (index >= 2 || path.empty()) {
        return false;
    }
    std::string re_path;
    auto        handle = const_cast<XXRouter<PermissionOperator, 2>&>(configDenyPermission_)
                      .get(std::string{path}, static_cast<int>(index), re_path, true);
    return handle != nullptr && *handle == PermissionOperator::DENY;
}

asio::awaitable<bool> PermissionMiddlewareHandle::defOnFilesystemHandle(
    const neograph::Tool& item,
    agentxx::util::Json&  args,
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
    // - worktree 子树 (allowPath) 内读写照常处理 (不参与下面的主检出写拒绝):
    //   该子树是本会话自己的工作区, 而真实 worktree 位于主检出的
    //   `.agentxx/agent/worktrees/` 下 (见 util::worktree::worktreesRoot),
    //   即 allowPath 本身就在 denyWritePath 子树内 —— 必须先于 denyWritePath
    //   判定, 否则会话对自身工作区的写操作也会被命中, 表现为绑定 worktree 后
    //   无法写任何文件
    // - 主检出子树 (denyWritePath) 内其余位置的写操作直接拒绝 (读不受限),
    //   保证多会话并行开发互不干扰; 拒绝以 tool 结果形式反馈给模型
    const SessionFsIsolation* iso = sessionIsolation(sessionId);
    const bool insideWorktree = iso && !iso->allowPath.empty() && isUnderDir(iso->allowPath, path);
    if (index == FilesystemPermissionWRITE && !insideWorktree) {
        if (iso && !iso->denyWritePath.empty() && isUnderDir(iso->denyWritePath, path)) {
            XX_LOGD(
                "Permission: session '{}' isolated by worktree, deny write to main checkout: {}",
                sessionId,
                path
            );
            co_return false;
        }
    }

    // 配置文件显式拒绝的路径: 无论后续是否完全授权, 始终保持拒绝且不询问
    if (isConfigDenied(path, index)) {
        XX_LOGD("Permission: path '{}' matches config deny rule, denied", path);
        co_return false;
    }

    // 若用户已完全授权所有权限: 允许任意权限访问, 不再询问
    if (isFullAuthorized()) {
        co_return true;
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
                co_return co_await requestPermission(item, args, index, path);
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
            co_return co_await requestPermission(item, args, index, path);
    }
    co_return true;
}

asio::awaitable<bool> PermissionMiddlewareHandle::requestPermission(
    const neograph::Tool& item,
    agentxx::util::Json&  args,
    size_t                index,
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
            .agentName = ctxPtr->agentConfig ? ctxPtr->agentConfig->agentName : std::string{},
            .sessionId = std::move(sessionId),
            .toolName  = item.get_name(),
            .category  = index == FilesystemPermissionREAD ? "filesystem_read" : "filesystem_write",
            .target    = target,
            .argumentsJson = args.dump(),
        },
        std::chrono::milliseconds{0} // 0 = 不限制
    );
    if (!resp.has_value()) {
        co_return false; // 无 prompter, 拒绝
    }
    const bool allow = resp->decision == events::RespPermission::Decision::Allow;
    // 记住本次选择: 应答者只回传用户意图 (RespPermission.remember), 规则表归本
    // 中间件所有, 因此在此注册 —— 目录规则按最长前缀匹配自动覆盖其全部子目录与
    // 文件 (如 /data/proj 的规则对 /data/proj/src/main.cpp 生效)
    // - 不能改由 IO 端点注册: 端点经会话总线 (session->bus) 发布规则事件, 而本
    //   中间件订阅的是 agent 全局总线 (agentContext->bus), 事件不会到达, 表现为
    //   "勾选记住后下次访问仍反复询问"
    if (resp->remember && !target.empty()) {
        setFilesystemPermission(
            target,
            allow ? PermissionOperator::ALLOW : PermissionOperator::DENY,
            index
        );
        XX_LOGI(
            "Permission: remembered {} rule for {} (index={})",
            allow ? "ALLOW" : "DENY",
            target,
            index
        );
    }
    // 选择启用"完全授权所有权限"并确认允许后, 切换为全授权状态,
    // 后续不再询问权限, 允许任意权限访问 (配置文件中拒绝的路径仍然保持拒绝)
    if (resp->fullAuth && allow) {
        setFullAuthorized(true);
        XX_LOGI("Permission: fully authorized all permissions (config denied paths remain denied)");
    }
    co_return allow;
}

void PermissionMiddlewareHandle::registerFilesystemHandles() {
    auto readHandle
        = [this](const neograph::Tool& item, agentxx::util::Json& args) -> asio::awaitable<bool> {
        co_return co_await defOnFilesystemHandle(item, args, FilesystemPermissionREAD);
    };

    handles["agentxx_filesystem_list"] = readHandle;
    handles["agentxx_filesystem_read"] = readHandle;
    handles["agentxx_filesystem_write"]
        = [this](const neograph::Tool& item, agentxx::util::Json& args) -> asio::awaitable<bool> {
        co_return co_await defOnFilesystemHandle(item, args, FilesystemPermissionWRITE);
    };
    handles["agentxx_filesystem_edit"]
        = [this](const neograph::Tool& item, agentxx::util::Json& args) -> asio::awaitable<bool> {
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

void PermissionMiddlewareHandle::registerOnBus(const std::shared_ptr<agentxx::event::EventBus>& bus
) {
    if (!bus) {
        return;
    }
    unregisterFromBus();
    registeredBus_ = bus;

    // 1. 注册权限检查服务端 (ReqToolPermissionCheck -> RespToolPermissionCheck)
    checkServerId_ = bus->getRR<events::ReqToolPermissionCheck, events::RespToolPermissionCheck>(
                            events::Topic::ToolPermissionCheck
    )
                         .registerServer(
                             [this](const events::ReqToolPermissionCheck& req, size_t)
                                 -> asio::awaitable<events::RespToolPermissionCheck> {
                                 auto it = handles.find(req.toolName);
                                 if (it != handles.end()) {
                                     DummyPermissionTool dummyTool(req.toolName);
                                     agentxx::util::Json argsCopy = req.arguments;
                                     auto allow = co_await it->second(dummyTool, argsCopy);
                                     co_return events::RespToolPermissionCheck{.allow = allow};
                                 }
                                 // 未注册权限拦截 handle 的普通工具直接放行
                                 co_return events::RespToolPermissionCheck{.allow = true};
                             }
                         );

    // 2. 订阅会话隔离设置事件 (EventSetSessionIsolation)
    // - 发布方: agentxx_git_worktree (工具, 经 ctx->bus 即本总线发布)
    // - 注意: 文件系统规则 ("记住本次选择"/白黑名单) 不经总线注入 —— 记住选择由
    //   询问应答 (RespPermission.remember) 在 [requestPermission] 内直接注册,
    //   配置规则由 BaseAgent 启动时直接注册; 二者都在本中间件内完成, 无需事件
    setIsolationSubId_
        = bus->get<events::EventSetSessionIsolation>(events::Topic::PermissionSetIsolation)
              .subscribe(
                  [this](const events::EventSetSessionIsolation& evt) -> asio::awaitable<void> {
                      SessionFsIsolation iso;
                      iso.allowPath     = normalizePermissionPath(evt.allowPath);
                      iso.denyWritePath = normalizePermissionPath(evt.denyWritePath);
                      setSessionIsolation(evt.sessionId, std::move(iso));
                      co_return;
                  }
              );

    // 3. 订阅会话隔离清除事件 (EventClearSessionIsolation)
    clearIsolationSubId_
        = bus->get<events::EventClearSessionIsolation>(events::Topic::PermissionClearIsolation)
              .subscribe(
                  [this](const events::EventClearSessionIsolation& evt) -> asio::awaitable<void> {
                      clearSessionIsolation(evt.sessionId);
                      co_return;
                  }
              );
}

void PermissionMiddlewareHandle::unregisterFromBus() {
    if (auto bus = registeredBus_.lock()) {
        if (checkServerId_ != 0) {
            bus->getRR<events::ReqToolPermissionCheck, events::RespToolPermissionCheck>(
                   events::Topic::ToolPermissionCheck
            )
                .unregisterServer(checkServerId_);
            checkServerId_ = 0;
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
