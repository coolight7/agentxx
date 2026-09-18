#include "agentxx/middlewares/permission.h"

#include "agentxx/event/event_stream.h"
#include "agentxx/event/events.h"
#include "utilxx_base/json.h"
#include "utilxx_base/string_util.h"
#include <cctype>
#include <filesystem>

namespace agentxx {
namespace middleware {

namespace {

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
    std::string absPath = utilxx_base::toCurrentSystemAbsolutePath(path, baseDir);
    std::string s       = utilxx_base::toUnixStandardPath(absPath);

    // 判断是否为目录:
    // 1. 在文件系统上实际存在且为目录
    // 2. 或文件系统上不存在, 但原路径末尾显式带有斜杠 (如 "dir/" 或 "dir\\") 表达目录意图
    // 对于普通文件 (或不存在且原路径无尾斜杠的文件路径), 绝不添加尾部 '/', 若有则去除
    std::error_code ec;
    auto            fsPath = utilxx_base::utf8ToPath(absPath);
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
    utilxx_base::toLowerSelf(s);
#endif
    return s;
}

void PermissionMiddlewareHandle::setSessionIsolation(
    std::string_view   sessionId,
    SessionFsIsolation isolation
) {
    // 与 setFilesystemPermission 同一口径: 传入路径先归一化 (绝对化 + Unix 分隔符
    // + Windows 转小写) 再存储。判定时被检查路径已按同一口径归一化, 若这里保留
    // 原始大小写 (如盘符 `D:` 与目录名大小写不同), 前缀比较会失配, 表现为隔离
    // 边界不生效 (主检出内写操作未被拒绝)
    isolation.allowPath     = normalizePermissionPath(isolation.allowPath, sessionId);
    isolation.denyWritePath = normalizePermissionPath(isolation.denyWritePath, sessionId);
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

/// 权限分类文本: 声明未指定 category 时按作用域生成
std::string_view PermissionMiddlewareHandle::defaultCategory(size_t scope) {
    return scope == FilesystemPermissionWRITE ? "filesystem_write" : "filesystem_read";
}

/// 声明工具权限限制 (插件在注册工具后调用; 见 plugin_api.h 的
/// agentxx.agent.permission 接口表)
void PermissionMiddlewareHandle::registerToolPermission(
    std::string_view   toolName,
    ToolPermissionSpec spec
) {
    if (toolName.empty()) {
        return;
    }
    toolPermissions_.insert_or_assign(std::string{toolName}, std::move(spec));
}

bool PermissionMiddlewareHandle::unregisterToolPermission(std::string_view toolName) {
    return toolPermissions_.erase(std::string{toolName}) > 0;
}

const ToolPermissionSpec* PermissionMiddlewareHandle::toolPermission(std::string_view toolName
) const {
    auto it = toolPermissions_.find(toolName);
    return it == toolPermissions_.end() ? nullptr : &it->second;
}

asio::awaitable<bool> PermissionMiddlewareHandle::checkToolPermission(
    std::string_view     toolName,
    utilxx_base::Json& args
) {
    // 未声明权限的工具不参与权限判定 (直接放行): 权限限制随工具来源 (插件) 走,
    // 未加载/未声明的工具与无权限需求一致
    const auto* spec = toolPermission(toolName);
    if (!spec) {
        co_return true;
    }
    co_return co_await checkToolPermission(toolName, args, *spec);
}

asio::awaitable<bool> PermissionMiddlewareHandle::checkToolPermission(
    std::string_view          toolName,
    utilxx_base::Json&      args,
    const ToolPermissionSpec& spec
) {
    // 无目标声明的工具: 工具级判定 (目标为空, 命中不到规则表, 由 noRuleOperator 兜底)
    if (spec.targetKind == ToolPermissionTargetKind::None || spec.targetArgs.empty()) {
        co_return co_await checkTargetPermission(toolName, args, spec.scope, {}, spec.category);
    }
    const auto sessionId = args.value("sessionId", std::string{});
    for (const auto& argName : spec.targetArgs) {
        // 目标值按参数实际 JSON 类型处理: 数组逐项判定 (如 glob 的 file_patterns),
        // 字符串视为单个目标; 数组为空/参数缺省时该参数不参与判定
        std::vector<std::string> rawTargets;
        if (args.contains(argName) && args[argName].is_array()) {
            rawTargets = utilxx_base::jsonGetStringArray(args, argName);
        } else {
            auto raw = args.value(argName, std::string{});
            if (!raw.empty()) {
                rawTargets.push_back(std::move(raw));
            }
        }
        for (const auto& raw : rawTargets) {
            // 路径目标按会话生效工作目录规范化为绝对路径 (与工具实际访问路径
            // 一致, 使注册的绝对路径规则也能匹配相对路径访问); 文本目标原样使用
            std::string target;
            if (spec.targetKind == ToolPermissionTargetKind::Path) {
                target = normalizePermissionPath(raw, sessionId);
            } else {
                target = raw;
            }
            // 参数缺省/为空的目标不参与判定 (如可选路径参数未提供):
            // 空目标命中不到任何规则, 判定结果恒为 noRuleOperator, 无意义
            if (target.empty()) {
                continue;
            }
            // 依次判定: 任一目标被拒绝即拒绝整个调用 (询问逐个进行)
            if (!co_await checkTargetPermission(
                    toolName,
                    args,
                    spec.scope,
                    target,
                    spec.category
                )) {
                co_return false;
            }
        }
    }
    co_return true;
}

asio::awaitable<bool> PermissionMiddlewareHandle::checkTargetPermission(
    std::string_view     toolName,
    utilxx_base::Json& args,
    size_t               index,
    std::string_view     target,
    std::string_view     category
) {
    const auto sessionId = args.value("sessionId", std::string{});
    if (target.empty()) {
        // 工具级判定 (无目标): 规则表按空目标查询恒不命中, 直接按 noRuleOperator 处理
        switch (noRuleOperator) {
            case PermissionOperator::ALLOW:
                co_return true;
            case PermissionOperator::DENY:
                co_return false;
            case PermissionOperator::INTERRUPT:
                co_return co_await requestPermission(
                    toolName,
                    args,
                    index,
                    std::string{},
                    category
                );
        }
        co_return true;
    }
    switch (decideTarget(target, index, sessionId)) {
        case PathDecision::Allow:
            co_return true;
        case PathDecision::Deny:
            co_return false;
        case PathDecision::Ask:
            // 需要询问: 经总线询问外部授权者 (CLI/GUI/ACP 各注册自己的 prompter)
            // - 无 prompter 注册时 request 返回 nullopt, 默认拒绝以保安全
            co_return co_await requestPermission(
                toolName,
                args,
                index,
                std::string{target},
                category
            );
    }
    co_return true;
}

PathDecision PermissionMiddlewareHandle::decideTarget(
    std::string_view path,
    size_t           index,
    std::string_view sessionId
) const {
    // TODO(符号链接穿透): 本判定基于词法规范化路径, 不解析符号链接 —— 允许范围
    // 内的链接 (如 <root>/link -> <root>/deny) 被读取/写入时会穿透到被拒目录,
    // 逐路径过滤接口也看不到链接目标 (枚举出的只是链接自身路径)。彻底处理需对
    // 已存在路径取 std::filesystem::weakly_canonical 后再判定一次 (影响所有工具
    // 与查询接口, 需评估性能与 Windows 语义), 暂不处理。
    if (path.empty()) {
        // 空目标无法判定: 不按"已批准"处理 (调用方按未获批准丢弃)
        return PathDecision::Ask;
    }
    // worktree 会话隔离边界 (优先于一切已注册规则):
    // - worktree 子树 (allowPath) 内读写照常处理 (不参与下面的主检出写拒绝):
    //   该子树是本会话自己的工作区, 而真实 worktree 位于主检出的
    //   `.agentxx/agent/worktrees/` 下 (见 utilxx::worktree::worktreesRoot),
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
            return PathDecision::Deny;
        }
    }

    // 配置文件显式拒绝的路径: 无论后续是否完全授权, 始终保持拒绝且不询问
    if (isConfigDenied(path, index)) {
        XX_LOGD("Permission: path '{}' matches config deny rule, denied", path);
        return PathDecision::Deny;
    }

    // 若用户已完全授权所有权限: 允许任意权限访问, 不再询问
    if (isFullAuthorized()) {
        return PathDecision::Allow;
    }

    std::string re_path;
    // 最长前缀匹配: 注册的文件夹规则 (如 /data/projects) 对其下任意子路径生效
    auto handle = const_cast<XXRouter<PermissionOperator, 2>&>(filesystemPermission)
                      .get(std::string{path}, static_cast<int>(index), re_path, true);
    if (nullptr != handle) {
        switch (*handle) {
            case PermissionOperator::ALLOW:
                return PathDecision::Allow;
            case PermissionOperator::DENY:
                return PathDecision::Deny;
            case PermissionOperator::INTERRUPT:
                return PathDecision::Ask;
        }
    }
    // 未命中任何规则: 按 noRuleOperator 处理 (CodeAgent 按 permission.mode 设置;
    // 默认 ALLOW 与历史行为一致, 无规则即放行)
    switch (noRuleOperator) {
        case PermissionOperator::ALLOW:
            return PathDecision::Allow;
        case PermissionOperator::DENY:
            return PathDecision::Deny;
        case PermissionOperator::INTERRUPT:
            return PathDecision::Ask;
    }
    return PathDecision::Allow;
}

std::vector<PathDecision> PermissionMiddlewareHandle::decidePaths(
    const std::vector<std::string>& paths,
    size_t                          index,
    std::string_view                sessionId
) const {
    std::vector<PathDecision> decisions;
    decisions.reserve(paths.size());
    for (const auto& raw : paths) {
        // 相对路径按会话生效工作目录规范化为绝对路径 (与工具实际访问路径、
        // 规则匹配口径一致); 规范化失败 (空路径) 按未获批准处理
        const auto normalized = normalizePermissionPath(raw, sessionId);
        decisions.push_back(
            normalized.empty() ? PathDecision::Ask : decideTarget(normalized, index, sessionId)
        );
    }
    return decisions;
}

asio::awaitable<bool> PermissionMiddlewareHandle::requestPermission(
    std::string_view     toolName,
    utilxx_base::Json& args,
    size_t               index,
    std::string          target,
    std::string_view     category
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
            .toolName  = std::string{toolName},
            // 分类文本: 工具声明的优先级高于按作用域生成的默认值
            .category      = std::string{category.empty() ? defaultCategory(index) : category},
            .target        = target,
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

PermissionMiddlewareHandle::~PermissionMiddlewareHandle() {
    unregisterFromBus();
}

void PermissionMiddlewareHandle::registerOnBus(const std::shared_ptr<agentxx::events::EventBus>& bus
) {
    if (!bus) {
        return;
    }
    unregisterFromBus();
    registeredBus_ = bus;

    // 1. 注册权限检查服务端 (ReqToolPermissionCheck -> RespToolPermissionCheck)
    // - 工具权限限制由工具来源方声明: 插件在注册工具后经 agentxx.agent.permission
    //   接口表声明 (见 PluginManager::registerToolPermission), 本中间件据此判定
    // - 未声明权限的工具不参与权限判定 (直接放行)
    checkServerId_ = bus->getRR<events::ReqToolPermissionCheck, events::RespToolPermissionCheck>(
                            events::Topic::ToolPermissionCheck
    )
                         .registerServer(
                             [this](const events::ReqToolPermissionCheck& req, size_t)
                                 -> asio::awaitable<events::RespToolPermissionCheck> {
                                 utilxx_base::Json argsCopy = req.arguments;
                                 auto allow = co_await checkToolPermission(req.toolName, argsCopy);
                                 co_return events::RespToolPermissionCheck{.allow = allow};
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
                      // 路径归一化由 setSessionIsolation 统一完成 (相对路径基准 =
                      // 该会话的工作目录, 避免在事件处理器里重复做一遍归一化)
                      SessionFsIsolation iso;
                      iso.allowPath     = evt.allowPath;
                      iso.denyWritePath = evt.denyWritePath;
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
