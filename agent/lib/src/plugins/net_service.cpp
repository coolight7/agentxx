/// 插件网络能力: `utilxx::NetHub` 的插件面适配 (实现)
///
/// 这里只做"属主 id 换名 / 事件换频道 / 状态位对齐"三件事; 真正的网络实现在
/// `cxx_utilxx` 的 `utilxx::NetHub` (HTTP / TCP / WebSocket / UDP, 自有网络线程)。
#include "agentxx/plugin/net_service.h"

#include "utilxx/net_hub.h"

#include "utilxx_base/json.h"
#include "utilxx_base/log.h"

#include <algorithm>
#include <mutex>
#include <unordered_set>

namespace agentxx {
namespace plugin {

namespace {

/// 把网络完成状态换成插件 ABI 的操作状态位
int32_t operatorStatusOf(utilxx::NetDoneStatus status) {
    switch (status) {
    case utilxx::NetDoneStatus::Ok:
        return PLUGINXX_OPERATOR_OK;
    case utilxx::NetDoneStatus::Canceled:
        return PLUGINXX_OPERATOR_CANCELED;
    default:
        return PLUGINXX_OPERATOR_FAILED;
    }
}

} // namespace

/// 服务内部状态: 在飞请求集合 + 与调用方的接口
struct PluginNetService::State {
    Hooks hooks;
    /// 在飞请求 (受理成功加入; 结算或按取消收尾时移除)
    std::mutex               mutex;
    std::unordered_set<int64_t> liveRequests;
};

PluginNetService::PluginNetService() : state_(std::make_shared<State>()) {}

std::shared_ptr<PluginNetService>
    PluginNetService::create(Hooks hooks, const std::string& dataDir) {
    auto service             = std::shared_ptr<PluginNetService>(new PluginNetService());
    service->state_->hooks   = std::move(hooks);

    // 事件: 网络线程上产出 → 投递回调用方线程再分发
    auto weakState = std::weak_ptr<State>(service->state_);
    utilxx::NetHub::PublishFn publish
        = [weakState](const std::string& owner, const std::string& payloadJson) {
              auto state = weakState.lock();
              if (!state || !state->hooks.publishEvent) {
                  return;
              }
              std::string ownerCopy   = owner;
              std::string payloadCopy = payloadJson;
              auto        emit        = [weakState, ownerCopy, payloadCopy]() {
                  auto alive = weakState.lock();
                  if (alive && alive->hooks.publishEvent) {
                      alive->hooks.publishEvent(ownerCopy, payloadCopy);
                  }
              };
              if (state->hooks.postToCaller) {
                  state->hooks.postToCaller(std::move(emit));
              } else {
                  emit();
              }
          };
    // 完成回调: 同样回到调用方线程执行
    utilxx::NetHub::PostFn post
        = [weakState](std::function<void()> fn) {
              auto state = weakState.lock();
              if (!state) {
                  return;
              }
              if (state->hooks.postToCaller) {
                  state->hooks.postToCaller(std::move(fn));
              } else {
                  fn();
              }
          };

    service->hub_ = utilxx::NetHub::create(std::move(publish), std::move(post));
    if (!dataDir.empty()) {
        service->hub_->setDataDir(dataDir);
    }
    service->hub_->start();
    return service;
}

PluginNetService::~PluginNetService() {
    stop();
}

void PluginNetService::stop() {
    if (hub_) {
        hub_->stop();
    }
    std::lock_guard lock(state_->mutex);
    state_->liveRequests.clear();
}

int32_t PluginNetService::call(
    std::string_view owner,
    std::string_view op,
    std::string_view argsJson,
    std::string*     outJson
) {
    if (!hub_) {
        return utilxx::net::ErrState;
    }
    std::string result;
    const int32_t rc = hub_->call(
        std::string{owner},
        std::string{op},
        std::string{argsJson},
        result
    );
    if (rc == utilxx::net::Ok && outJson != nullptr) {
        *outJson = std::move(result);
    }
    return rc;
}

int32_t PluginNetService::callAsync(
    std::string_view         owner,
    std::string_view         op,
    std::string_view         argsJson,
    PluginxxOperatorCallback cb,
    void*                    ud,
    std::string*             outJson,
    int64_t*                 outRequestId,
    std::weak_ptr<void>      guard
) {
    if (!hub_) {
        return utilxx::net::ErrState;
    }
    auto weakState = std::weak_ptr<State>(state_);
    // 空弱引用 = "不检查实例死活" (owner_before 两向都为假判它是空)
    const std::weak_ptr<void> emptyWeak{};
    const bool                hasGuard = guard.owner_before(emptyWeak)
                        || emptyWeak.owner_before(guard);
    auto done = [weakState, cb, ud, guard, hasGuard](
                    utilxx::NetDoneStatus status,
                    const std::string&    payloadJson
                ) {
        auto state = weakState.lock();
        if (!state) {
            return;
        }
        // 插件实例已经销毁: 不再回到插件代码 (这次请求按取消处理)
        if (hasGuard && guard.expired()) {
            return;
        }
        if (cb == nullptr) {
            return;
        }
        PluginxxStringView view{};
        if (!payloadJson.empty()) {
            view.data = payloadJson.data();
            view.size = static_cast<uint64_t>(payloadJson.size());
        }
        cb(ud, operatorStatusOf(status), payloadJson.empty() ? nullptr : &view);
    };

    std::string   accepted;
    int64_t       requestId = 0;
    const int32_t rc        = hub_->callAsync(
        std::string{owner},
        std::string{op},
        std::string{argsJson},
        std::move(done),
        accepted,
        &requestId
    );
    if (rc == utilxx::net::Ok) {
        {
            std::lock_guard lock(state_->mutex);
            state_->liveRequests.insert(requestId);
        }
        if (outRequestId != nullptr) {
            *outRequestId = requestId;
        }
        if (outJson != nullptr) {
            *outJson = std::move(accepted);
        }
    } else if (outJson != nullptr) {
        *outJson = std::move(accepted);
    }
    return rc;
}

int32_t PluginNetService::cancel(
    std::string_view owner,
    int64_t          requestId,
    std::string*     outJson
) {
    if (!hub_) {
        return utilxx::net::ErrState;
    }
    std::string   result;
    int32_t       rc = hub_->cancel(std::string{owner}, requestId, result);
    if (rc == utilxx::net::Ok) {
        // 取消后由网络侧给出 Canceled 完成回调, 这里只记一笔"已不可再取消"
        {
            std::lock_guard lock(state_->mutex);
            state_->liveRequests.erase(requestId);
        }
        // 网络侧把"没有这个请求"写在结果 JSON 里; 插件面契约是参数层错误码
        try {
            auto parsed = utilxx_base::Json::parse(result);
            if (parsed.is_object() && parsed.contains("ok") && !parsed["ok"].get<bool>()
                && parsed.value("error", std::string{}) == "not_found") {
                rc = utilxx::net::ErrNotFound;
            }
        } catch (...) {
            // 结果不是 JSON: 保持原状态码
        }
    }
    if (outJson != nullptr) {
        *outJson = std::move(result);
    }
    return rc;
}

void PluginNetService::shutdownOwner(std::string_view owner) {
    if (!hub_) {
        return;
    }
    const std::string ownerId{owner};
    auto              pendings = hub_->shutdownOwner(ownerId);
    {
        std::lock_guard lock(state_->mutex);
        state_->liveRequests.clear();
    }
    // 未完成的请求按"已取消"结算: 回调就在这里 (调用方线程) 执行
    for (auto& done : pendings) {
        if (done) {
            done(utilxx::NetDoneStatus::Canceled, std::string{});
        }
    }
    if (!pendings.empty()) {
        XX_LOGI("Plugin `{}` net: {} pending request(s) canceled on shutdown", ownerId, pendings.size());
    }
}

} // namespace plugin
} // namespace agentxx
