/// agentxx 功能点 (feature point) 子系统 —— 点 / 实现 / 调用三层
///
/// 背景: 核心在关键位置留出的扩展点 (token 估算 / 上下文压缩 / 工具输出摘要 ...)
/// 过去写死在中间件与节点里, 插件既提供不了也调用不了, 也没有统一的清单、顺序、
/// 置空与调用口径。本子系统把这些扩展点统一成"功能点":
///
/// - **点 (Point)**: 一个带 id 的对象 ([ProvidePoint] / [JsonProvidePoint]), 自己
///   持有实现列表、值缓存、统计与说明; 用哪个类由代码决定, `type` 只用于展示与日志;
/// - **实现 (Impl)**: 分两层 —— `plugin` (插件与 FFI 宿主登记的实现) → `core`
///   (libagentxx 自身实现); 层内按 `(priority 升序, 登记顺序)`。埋点天生接受外部
///   实现 (没有"只允许核心实现"这类开关), `core` 实现只是"没人给值时的兜底";
/// - **调用 (Call)**: 应用自己用 [ProvidePoint::ask] (读值缓存、记置空), 外部
///   (插件 / FFI 宿主 / 命令行) 用 [ProvidePoint::call] (只读值缓存、不记置空、
///   不写调用方会话、不发界面提示、不落盘)。
///
/// 关键约定 (实现时不要绕开):
/// - **顺序必须声明**: 同层内按 `priority` 升序, 同优先级才退回登记顺序;
///   `priority` 越界裁剪到上下限并记一条警告 (不拒绝登记);
/// - **显式置空**: 实现回答 `{"disable": true}` 表示"这一次不再问后面的实现";
///   `ask` 记下置空标记 (清单里能看到 `disabledBy`), `call` 不记;
/// - **值缓存由发起方决定**: `ask` 拿到值写缓存, `call` 不写; 同一身份并发只跑一次
///   实现 (in-flight 去重), 写不写缓存按发起方算;
/// - **超时两处都实现、默认都不限 (0)**: 点的声明方可以给 `implTimeoutMs` (等单个
///   插件实现, 到点取消当次实现并按"没意见"继续链), 调用方可以给 `timeoutMs`
///   (等整次调用, 到点返回 `failed`);
/// - **全异步**: 实现与调用都在宿主 io 线程上跑协程; 不提供同步等待入口。
///
/// 统计与成功调用日志只在开发者模式 ([agentxx::agent::AgentConfigStatic::devMode],
/// 启动期冻结) 下收集; 关闭时清单只给"当前状态", 不含 `stat` 段。
#pragma once

#include "agentxx/feature/value_cache.h"
#include "agentxx/util/exception.h"
#include "utilxx/async_offload.h"
#include "utilxx_base/json.h"
#include "utilxx_base/log.h"
#include "asio/awaitable.hpp"
#include "asio/redirect_error.hpp"
#include "asio/steady_timer.hpp"
#include "asio/use_awaitable.hpp"
#include "fmt/format.h"
#include <algorithm>
#include <chrono>
#include <concepts>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace agentxx {
namespace feature {

// ==================== 常量与枚举 ====================

/// 功能点的类型 (本轮只实现 provide; decide 只有核心点使用; notify 预留)
enum class PointType : int32_t {
    Provide = 0, ///< 取值: 请求 → 值, 第一个给出合法值的实现生效
    Decide  = 1, ///< 裁决: 通过 / 拒绝 / 弃权 (本轮只有核心点使用)
    Notify  = 2, ///< 预留: 通知型 (当前由事件表与固定钩子承担)
};

/// 实现所在的层 (排序时 `plugin` 恒在 `core` 之前)
enum class ImplLayer : int32_t {
    Plugin = 0, ///< 插件与 FFI 宿主登记的实现
    Core   = 1, ///< libagentxx 自身实现 (没人给值时的兜底)
};

/// 调用结果错误码 (对外字符串见 [callErrorKey], 改动即改契约)
enum class CallError : int32_t {
    None        = 0, ///< 成功
    NotCallable = 1, ///< 点没有对外开放调用
    BadArgs     = 2, ///< 参数不合法 (调用方给的不是合法 JSON / 形状不符)
    NoImpl      = 3, ///< 现在没有实现给出值
    Disabled    = 4, ///< 被某个实现显式置空 (`{"disable": true}`)
    Busy        = 5, ///< 同一 (点, 调用方) 上一次还没结束
    Failed      = 6, ///< 实现抛异常 / 结果不合法 / 调用方自己的超时
};

/// 类型 / 层 / 错误码的对外字符串 (`type` 字段、清单与结果 JSON 用它)
const char* pointTypeKey(PointType type) noexcept;
const char* implLayerKey(ImplLayer layer) noexcept;
const char* callErrorKey(CallError error) noexcept;

/// `priority` 允许范围: 越界裁剪到上下限并记一条警告 (不拒绝登记)
/// - 上限刻意远大于宿主默认带 1000: 声明方显式给更大值就能排在宿主实现之后
/// - 下限对称: 插件想抢在所有人之前可用负值
inline constexpr int32_t kPriorityMin = -100000;
inline constexpr int32_t kPriorityMax = 100000;
/// 默认优先级带: 插件实现 `0`, FFI 宿主实现 `1000` (排在插件之后、core 之前)
inline constexpr int32_t kPluginDefaultPriority = 0;
inline constexpr int32_t kHostDefaultPriority   = 1000;
/// 单次请求 / 结果的大小上限 (超过直接拒绝并说明)
inline constexpr size_t kFeatureMaxPayloadBytes = 8u * 1024u * 1024u;

// ==================== 点的声明与实现登记 ====================

/// 点的声明选项
struct PointOptions {
    /// 展示名 (缺省用 id)
    std::string title;
    /// 一句话说明
    std::string depict;
    /// 参数与结果说明 (空 = 不对外开放调用; 文档与命令帮助共用这份文本)
    std::string callDoc;
    /// 是否允许被外部主动调用 (默认不可调; 插件声明的点在其声明期间可调)
    bool callable = false;
    /// 值缓存策略 (默认不缓存)
    CacheMode cache = CacheMode::None;
    /// 条数上限 (0 = 内置默认 [kDefaultCacheMaxItems]; 仅 `ByIdentity` 生效)
    size_t maxItems = 0;
    /// 字节上限 (0 = 内置默认 [kDefaultCacheMaxBytes])
    size_t maxBytes = 0;
    /// 等单个插件实现的超时 (毫秒; 0 = 不限, 默认)
    int32_t implTimeoutMs = 0;
};

/// 实现在运行期收到的调用上下文 (核心实现直接拿到它)
struct ImplContext {
    std::string point;           ///< 功能点 id
    std::string owner;           ///< 本实现的归属 (`plugin:<名>` / `host:<名>` / `core:<域>:<实现>`)
    std::string caller;          ///< 调用方 (空串 = 应用自己)
    bool        viaCall = false; ///< true = 外部主动调用 (零副作用契约)
    std::string identity;        ///< 本次身份
    std::string sessionId;       ///< 规范请求里带的会话 (可为空)
};

/// 实现登记规格 (插件 / FFI 宿主用; 核心实现走 [ProvidePoint::addCoreImpl])
struct ImplSpec {
    /// 调用实现: 入参为调用上下文 JSON 文本, 返回回答文本 (nullopt = 没意见)
    /// - 回答形状见 [parseImplAnswer]: `{"value": ...}` / `{"disable": true}` / `{}`
    /// - 实现抛异常等价于"没意见" (宿主记一条警告并继续问下一个实现)
    using Fn = std::function<asio::awaitable<std::optional<std::string>>(std::string callJson)>;
    /// 请求取消当次实现 (可为空); 到点超时或调用方放弃时由宿主调用
    using CancelFn = std::function<void()>;

    ImplLayer   layer = ImplLayer::Plugin;
    std::string owner;                ///< 归属标签 (清单里显示)
    int32_t     priority         = kPluginDefaultPriority;
    int32_t     defaultTimeoutMs = 0; ///< 实现自报的最大耗时 (0 = 不限)
    std::string load;                 ///< `dynamic` / `builtin` (仅展示, 不影响顺序)
    std::string note;                 ///< 备注 (清单里显示)
    Fn          fn;
    CancelFn    cancel;
};

/// 一次调用在各层之间传递的上下文 (点内部使用)
struct ChainEnv {
    std::string requestJson; ///< 应用点解析后的规范请求 (插件点为空对象)
    std::string argsJson;    ///< 传给实现的参数 (核心点 = 规范请求; 插件点 = 调用方原样参数)
    std::string identity;    ///< 本次身份
    std::string caller;      ///< 调用方 (空串 = 应用自己)
    std::string sessionId;
    bool        viaCall = false;
    /// 强类型请求的借用指针 (`ProvidePoint` 自己设置与读取; 核心实现据此跳过 JSON)
    /// - 指向调用方传进来的请求对象, 生命周期覆盖本次 await
    const void* typedRequest = nullptr;
};

/// 实现链的结果 (点内部使用; 值一律是 JSON 文本)
struct ChainOutcome {
    bool        hasValue = false;
    bool        disabled = false;
    std::string valueJson;
    std::string by;      ///< 给出值 / 置空的实现归属
    std::string message; ///< 无值时的可读说明
    CallError   error = CallError::NoImpl;
};

/// 一次取值 / 调用的结果 (封装前, 值一律是 JSON 文本)
struct ResolvedValue {
    CallError   error = CallError::NoImpl;
    std::string valueJson;
    std::string by;
    std::string identity;
    bool        fromCache = false;
    int64_t     ms        = 0;
    std::string message;

    /// 是否拿到了值
    bool ok() const noexcept {
        return error == CallError::None;
    }
};

/// `ask` 的选项 (应用自己取值)
struct AskOptions {
    /// 显式身份 (空 = 由点自己的身份规则算)
    std::string identity;
    /// 会话 (进实现收到的调用上下文; 空 = 不带)
    std::string sessionId;
    /// 跳过值缓存重算
    bool refresh = false;
};

/// `call` 的选项 (外部主动调用)
struct CallOptions {
    /// 调用方归属 (`plugin:<名>` / `host:<名>` / 命令行等; 空 = 应用自己)
    std::string caller;
    /// 显式身份 (空 = 由点自己的身份规则算)
    std::string identity;
    /// 会话 (进实现收到的调用上下文; 空 = 不带)
    std::string sessionId;
    /// 跳过值缓存重算 (仍然不写缓存)
    bool refresh = false;
    /// 调用方超时 (0 = 不限; 到点返回 `failed` 并取消当次实现)
    std::chrono::milliseconds timeout{0};
};

/// 一次取值 / 调用的输入 (点内部使用)
struct ResolveRequest {
    std::string requestJson;
    std::string argsJson;
    std::string identity;
    std::string caller; ///< 空 = 应用自己 (`ask`)
    std::string sessionId;
    bool        viaCall = false;
    /// 拿到值写不写值缓存 (`ask` = true, `call` = false)
    bool persist = true;
    /// 置空记不记标记 (`ask` = true, `call` = false)
    bool rememberDisable = true;
    /// 跳过值缓存重算
    bool refresh = false;
    /// 调用方超时 (0 = 不限)
    std::chrono::milliseconds timeout{0};
    /// 强类型请求的借用指针 (见 [ChainEnv::typedRequest])
    const void* typedRequest = nullptr;
};

/// 点的调用结果 (封装后; `ask` 与 `call` 共用一份形状)
template<class TValue>
struct FeatureResult {
    CallError             error = CallError::None;
    std::optional<TValue> value;
    std::string           by;       ///< 谁给的 (`core:<域>:<实现>` / `plugin:<名>` / `host:<名>`)
    std::string           identity; ///< 本次身份 (调用方可据此自己缓存)
    bool                  fromCache = false;
    int64_t               ms        = 0;
    std::string           message;  ///< 失败时一定有可读说明

    /// 是否拿到值
    bool ok() const noexcept {
        return error == CallError::None;
    }
};

/// `ask` 的结果 (应用自己取值)
template<class TValue>
using AskResult = FeatureResult<TValue>;
/// `call` 的结果 (外部主动调用)
template<class TValue>
using CallResult = FeatureResult<TValue>;

/// 通用 JSON 值 (插件点与宿主点用)
using JsonResult = FeatureResult<utilxx_base::Json>;

// ==================== 类型化的 JSON 编解码 ====================

/// 请求与值的 JSON 编解码 (按类型特化)
/// - 核心点在自己的头里给出特化 (示例见 agentxx/feature/points.h)
/// - 未特化的类型不能作为功能点的请求 / 值 (编译期报"使用了被删除的函数")
template<class T>
struct Codec {
    static std::string      toJson(const T&)           = delete;
    static std::optional<T> fromJson(std::string_view) = delete;
};

/// 该类型是否可用作功能点的请求 / 值
template<class T>
concept JsonCodable = requires(const T& v, std::string_view s) {
    { Codec<T>::toJson(v) } -> std::convertible_to<std::string>;
    { Codec<T>::fromJson(s) } -> std::convertible_to<std::optional<T>>;
};

/// 实现回答的解析结果 (插件层与核心层共用同一份口径)
struct ImplAnswer {
    enum class Kind : int32_t {
        NoOpinion = 0, ///< 没意见 (空对象 / 不认识的形状)
        Value     = 1, ///< 给了值 (`value` 为 null 也算"算过了, 结果是空")
        Disable   = 2, ///< 显式置空
        Verdict   = 3, ///< 裁决型回答 (decide 点; 本轮只有核心点使用)
    };

    Kind        kind = Kind::NoOpinion;
    std::string valueJson; ///< Kind::Value 时的值 (JSON 文本)
    std::string verdict;   ///< Kind::Verdict 时的取值 (`deny` / `abstain` / `allow`)
    std::string reason;
};

/// 解析实现给出的回答文本 (形状见 [ImplSpec::Fn] 说明)
/// - 非法 JSON 等价于"没意见" (调用方记一条警告后继续问下一个)
ImplAnswer parseImplAnswer(std::string_view text);

// ==================== 点基类 (类型擦除部分) ====================

/// 功能点基类: id / 选项 / 插件层实现链 / 值缓存 / 置空标记 / 统计 / 清单
///
/// 类型化的取值逻辑在派生类 ([ProvidePoint] / [JsonProvidePoint]); 这里只保留
/// "与请求类型无关"的部分, 使注册表、清单与插件面都能按 id 统一处理。
class PointBase {
public:

    PointBase(std::string id, PointType type, PointOptions opts);
    virtual ~PointBase();

    PointBase(const PointBase&)            = delete;
    PointBase& operator=(const PointBase&) = delete;

    const std::string&  id() const noexcept;
    PointType           type() const noexcept;
    const PointOptions& options() const noexcept;
    /// 声明来源: `core` / `plugin:<插件名>` / `host:<宿主名>`
    const std::string& origin() const noexcept;
    void               setOrigin(std::string origin);

    /// 是否允许被外部主动调用
    bool    callable() const noexcept;
    /// 等单个插件实现的超时 (毫秒; 0 = 不限)
    int32_t implTimeoutMs() const noexcept;

    // ---- 实现登记 (插件 / FFI 宿主) ----
    /// 登记实现
    /// - 同一 `(点, owner)` 重复登记为覆盖 (记一条日志)
    /// - `priority` 越界裁剪到上下限并记一条警告 (不拒绝登记)
    /// - 点的个数、每个点的实现数都不设上限 (非必要不加检查与限制)
    ///
    /// - `args`: [_spec] 实现规格 (层 / 归属 / 优先级 / 调用函数)
    ///
    /// `return`: 0 成功; 非 0 失败 (id 或调用函数为空)
    int32_t addImpl(ImplSpec spec);

    /// 撤销某归属的实现 (插件注销 / 禁用 / 卸载时由宿主调用)
    /// - 顺带清掉它产出的值缓存条目与它留下的置空标记
    ///
    /// `return`: 撤销的实现条数
    size_t removeImpls(std::string_view owner);

    /// 某归属是否在本点登记了实现
    bool hasImplOf(std::string_view owner) const;

    /// 插件层实现条数
    size_t implCount() const noexcept;

    /// 当前会生效的实现 (层 + 优先级最小的那个) 的归属; 无实现返回空串
    std::string effectiveBy() const;

    /// 最近一次 `ask` 被谁置空 (空 = 没有置空; `call` 不写这个标记)
    const std::string& disabledBy() const noexcept;
    /// 清掉置空标记 (点被重新声明 / 产出方被摘除时)
    void clearDisabled();

    /// 清空全部实现与状态 (卸载全部插件时用)
    void clearImpls();

    /// 值缓存条目数 / 占用字节 / 命中次数 (诊断与测试)
    size_t   cacheSize() const noexcept;
    size_t   cacheBytes() const noexcept;
    uint64_t cacheHits() const noexcept;

    /// 按来源失效值缓存 (实现被摘除 / 插件禁用卸载)
    ///
    /// `return`: 清除的条目数
    size_t invalidateCacheOf(std::string_view owner);

    /// 单点的清单 JSON (字段见设计文档 §11.1)
    utilxx_base::Json listJson() const;

    /// 运行入口: 值缓存 → in-flight 去重 → 实现链 → 按发起方决定写缓存 / 记置空
    asio::awaitable<ResolvedValue> runResolve(ResolveRequest req);

    // ---- 统计 (只在开发者模式下累加) ----
    /// 记一次 `ask` / `call` / 结果 (开发者模式关闭时这些函数只做一次只读判断)
    void noteAsk() noexcept;
    void noteCall() noexcept;
    void noteOutcome(bool ok, std::string_view by, std::string_view caller, int64_t ms) noexcept;

protected:

    /// 一条已登记的实现
    struct ImplEntry {
        ImplLayer          layer = ImplLayer::Plugin;
        std::string        owner;
        int32_t            priority         = kPluginDefaultPriority;
        int32_t            defaultTimeoutMs = 0;
        uint64_t           seq              = 0;
        std::string        load;
        std::string        note;
        ImplSpec::Fn       fn;
        ImplSpec::CancelFn cancel;
    };

    /// 调用统计 (只在开发者模式下累加; 无历史数组, 固定大小)
    struct PointStats {
        uint64_t    asks   = 0;
        uint64_t    calls  = 0;
        uint64_t    errors = 0;
        std::string lastBy;
        std::string lastCaller;
        int64_t     lastMs = 0;
    };

    /// in-flight 去重的一次计算 (同一身份并发共用)
    struct InFlight {
        bool         done = false;
        ChainOutcome outcome;
        /// 等待者各自的定时器 (发起方完成时统一唤醒)
        std::vector<std::shared_ptr<asio::steady_timer>> waiters;
    };

    /// 实现链: 先 `plugin` 层 (本类提供的 [runPluginLayer]), 再由派生类问 `core` 层
    virtual asio::awaitable<ChainOutcome> runChain(const ChainEnv& env) = 0;

    /// 插件层实现 (JSON 实现): 按 `(priority, 登记顺序)` 依次问
    /// - `call` 时跳过调用方自己的实现 (保护 ②: 不把自己的实现又问回自己)
    asio::awaitable<ChainOutcome> runPluginLayer(const ChainEnv& env);

    /// 问单个插件层实现 (含 `implTimeoutMs` 与实现自报超时; 异常按"没意见"处理)
    asio::awaitable<std::optional<ChainOutcome>>
        runOneImpl(const ImplEntry& entry, const ChainEnv& env);

    /// 调一次实现并做异常隔离 (抛异常 = 没意见, 记一条警告)
    /// - `callJson` 按值传入 (实现可能挂起, 载荷必须活到它结束)
    asio::awaitable<std::optional<std::string>>
        invokeImpl(const ImplEntry& entry, std::string callJson);

    /// 组装实现收到的调用上下文 JSON
    std::string buildCallJson(const ImplEntry& entry, const ChainEnv& env) const;

    /// 把实现链结果落到 [ResolvedValue] (含写缓存 / 记置空)
    ResolvedValue finalize(const ChainOutcome& outcome, const ResolveRequest& req, int64_t ms);

    /// 排好序的插件层实现 (层 + 优先级 + 登记顺序)
    std::vector<const ImplEntry*> orderedPluginImpls() const;

    /// 清空置空标记与值缓存 (点被重新声明)
    void resetState();

    /// 清单里的一条实现 (插件层与核心层共用一份展示形状)
    struct ImplView {
        ImplLayer   layer = ImplLayer::Plugin;
        std::string owner;
        int32_t     priority      = 0;
        int32_t     defaultTimeoutMs = 0;
        uint64_t    seq           = 0;
        std::string load;
        std::string note;
    };

    /// 当前全部实现的展示视图 (基类给插件层; 派生类追加自己的核心层实现)
    virtual std::vector<ImplView> implViews() const;

    /// in-flight 表项完成后发布结果并唤醒等待者
    void publishInFlight(
        const std::string&                identity,
        const std::shared_ptr<InFlight>&  slot,
        ChainOutcome                      outcome
    );

    std::string                              id_;
    PointType                                type_ = PointType::Provide;
    PointOptions                             opts_;
    std::string                              origin_;
    std::vector<ImplEntry>                   impls_;
    uint64_t                                 seqCounter_ = 0;
    std::string                              disabledBy_;
    ValueCache                               cache_;
    PointStats                               stats_;
    std::set<std::string, std::less<>>       activeCallers_;
    std::map<std::string, std::shared_ptr<InFlight>, std::less<>> inFlight_;
};

// ==================== 取值型功能点 (核心侧强类型) ====================

/// 取值型功能点: 请求 `TReq` → 值 `TValue`
///
/// - 核心实现的签名是强类型的 (不经过 JSON), 因此热路径上可以零拷贝;
///   跨插件边界仍一律 JSON (插件层实现收到调用上下文 JSON 文本);
/// - 值的 JSON 形态由 [Codec] 决定, 值缓存里存的就是这份 JSON 文本。
template<class TReq, class TValue>
    requires JsonCodable<TReq> && JsonCodable<TValue>
class ProvidePoint : public PointBase {
public:

    using ReqType   = TReq;
    using ValueType = TValue;
    /// 核心实现: 返回 nullopt 表示"没意见, 继续问下一个"
    using CoreImplFn =
        std::function<asio::awaitable<std::optional<TValue>>(const TReq&, const ImplContext&)>;
    /// 身份规则: 由点声明"这次要算的是哪一份输入"
    using IdentityFn = std::function<std::string(const TReq&)>;

    ProvidePoint(std::string id, PointOptions opts, IdentityFn identityOf) :
        PointBase(std::move(id), PointType::Provide, std::move(opts)),
        identityOf_(std::move(identityOf)) {}

    /// 登记核心实现 (层 `core`)
    /// - `owner` 形如 `core:<域>:<实现>`; 同一 `owner` 重复登记为覆盖
    /// - 核心实现不参与 `implTimeoutMs` (同进程直调, 不涉及跨边界等待)
    void addCoreImpl(std::string owner, int32_t priority, CoreImplFn fn) {
        for (auto& item : coreImpls_) {
            if (item.owner == owner) {
                item.priority = clampPriority(priority);
                item.fn       = std::move(fn);
                return;
            }
        }
        coreImpls_.push_back(CoreImpl{std::move(owner), clampPriority(priority), std::move(fn)});
        std::stable_sort(coreImpls_.begin(), coreImpls_.end(), [](const CoreImpl& a, const CoreImpl& b) {
            return a.priority < b.priority;
        });
    }

    /// 撤销核心实现 (按归属)
    ///
    /// `return`: 撤销的条数
    size_t removeCoreImpls(std::string_view owner) {
        const size_t before = coreImpls_.size();
        std::erase_if(coreImpls_, [owner](const CoreImpl& item) {
            return item.owner == owner;
        });
        invalidateCacheOf(owner);
        if (disabledBy_ == owner) {
            clearDisabled();
        }
        return before - coreImpls_.size();
    }

    /// 是否有核心实现
    bool hasCoreImpl() const noexcept {
        return !coreImpls_.empty();
    }

    /// 应用自己取值: 读值缓存 → 跑实现链 → 写值缓存、记置空
    asio::awaitable<AskResult<TValue>> ask(const TReq& req, AskOptions opts = {}) {
        noteAsk();
        ResolveRequest rr;
        fillRequest(rr, req);
        rr.identity        = opts.identity.empty() ? identityOf_(req) : opts.identity;
        rr.sessionId       = opts.sessionId;
        rr.persist         = true;
        rr.rememberDisable = true;
        rr.refresh         = opts.refresh;
        auto rv            = co_await runResolve(std::move(rr));
        co_return decodeResult(rv);
    }

    /// 外部主动调用: 只读值缓存、不记置空、不写值缓存
    asio::awaitable<CallResult<TValue>> call(const TReq& req, CallOptions opts = {}) {
        noteCall();
        ResolveRequest rr;
        fillRequest(rr, req);
        rr.identity        = opts.identity.empty() ? identityOf_(req) : opts.identity;
        rr.caller          = opts.caller;
        rr.sessionId       = opts.sessionId;
        rr.viaCall         = true;
        rr.persist         = false;
        rr.rememberDisable = false;
        rr.refresh         = opts.refresh;
        rr.timeout         = opts.timeout;
        auto rv            = co_await runResolve(std::move(rr));
        co_return decodeResult(rv);
    }

    /// 身份规则 (调用方不显式给身份时使用)
    const IdentityFn& identityOf() const noexcept {
        return identityOf_;
    }

protected:

    /// 追加核心层实现到清单视图 (跨层的排序由基类按 (层, 优先级, 登记顺序) 统一处理)
    std::vector<ImplView> implViews() const override {
        auto views = PointBase::implViews();
        uint64_t seq = 0;
        for (const auto& item : coreImpls_) {
            views.push_back(ImplView{
                ImplLayer::Core,
                item.owner,
                item.priority,
                0,
                ++seq,
                "builtin",
                "libagentxx 核心实现",
            });
        }
        return views;
    }

    asio::awaitable<ChainOutcome> runChain(const ChainEnv& env) override {
        // 1) 插件层 (插件 / FFI 宿主登记的实现)
        auto outcome = co_await runPluginLayer(env);
        if (outcome.hasValue || outcome.disabled) {
            co_return outcome;
        }

        // 2) core 层 (强类型实现); 请求解码失败等价于"没有核心实现"
        const TReq* reqPtr = static_cast<const TReq*>(env.typedRequest);
        std::optional<TReq> decoded;
        if (reqPtr == nullptr) {
            decoded = Codec<TReq>::fromJson(env.requestJson);
            if (!decoded.has_value()) {
                XX_LOGW("功能点 `{}`: 请求解码失败, 跳过核心实现", id_);
                co_return outcome;
            }
            reqPtr = &*decoded;
        }
        for (const auto& item : coreImpls_) {
            if (!item.fn) {
                continue;
            }
            ImplContext ictx{
                id_,
                item.owner,
                env.caller,
                env.viaCall,
                env.identity,
                env.sessionId,
            };
            const std::string&    ownerRef = item.owner;
            const CoreImplFn&     fnRef    = item.fn;
            const TReq&           reqRef   = *reqPtr;
            std::optional<TValue> value
                = co_await agentxx::util::catchErrorAsync<std::optional<TValue>>(
                    [&fnRef, &reqRef, &ictx]() -> asio::awaitable<std::optional<TValue>> {
                        co_return co_await fnRef(reqRef, ictx);
                    },
                    [this, &ownerRef](std::string errmsg) -> asio::awaitable<std::optional<TValue>> {
                        // 实现抛异常只等于"没意见": 记一条警告后继续问下一个实现
                        XX_LOGW(
                            "功能点 `{}` 的核心实现 `{}` 失败, 按没意见继续: {}",
                            id_,
                            ownerRef,
                            errmsg
                        );
                        co_return std::nullopt;
                    }
                );
            if (value.has_value()) {
                ChainOutcome out;
                out.hasValue  = true;
                out.valueJson = Codec<TValue>::toJson(*value);
                out.by        = ownerRef;
                out.error     = CallError::None;
                co_return out;
            }
        }
        co_return outcome;
    }

private:

    struct CoreImpl {
        std::string owner;
        int32_t     priority = 0;
        CoreImplFn  fn;
    };

    /// 填请求: 强类型指针给核心实现用; 请求 JSON 只在存在插件层实现时才生成
    /// (没有插件实现时省掉整段请求的序列化, 热路径上很值钱)
    void fillRequest(ResolveRequest& rr, const TReq& req) const {
        rr.typedRequest = &req;
        if (implCount() > 0) {
            rr.requestJson = Codec<TReq>::toJson(req);
            rr.argsJson    = rr.requestJson;
        }
    }

    /// `priority` 越界裁剪到上下限 (与插件层同一口径)
    static int32_t clampPriority(int32_t priority) noexcept {
        if (priority < kPriorityMin) {
            return kPriorityMin;
        }
        if (priority > kPriorityMax) {
            return kPriorityMax;
        }
        return priority;
    }

    /// 把 JSON 形态的结果解码成强类型结果
    FeatureResult<TValue> decodeResult(const ResolvedValue& rv) const {
        FeatureResult<TValue> out;
        out.error     = rv.error;
        out.by        = rv.by;
        out.identity  = rv.identity;
        out.fromCache = rv.fromCache;
        out.ms        = rv.ms;
        out.message   = rv.message;
        if (!rv.ok()) {
            return out;
        }
        auto value = Codec<TValue>::fromJson(rv.valueJson);
        if (!value.has_value()) {
            out.error   = CallError::Failed;
            out.message = fmt::format(
                "功能点 `{}` 的实现返回了无法解码的值: {}",
                id_,
                std::string_view{rv.valueJson}.substr(0, 200)
            );
            XX_LOGW("{}", out.message);
            return out;
        }
        out.value = std::move(*value);
        return out;
    }

    IdentityFn            identityOf_;
    std::vector<CoreImpl> coreImpls_;
};

// ==================== 通用 JSON 取值点 ====================

/// 通用 JSON 取值点: 请求与值都是任意 JSON
///
/// - 用于插件声明的点 (`plugin.<自己>.<名字>`) 与宿主声明的点; 参数原样交给实现;
/// - 插件点本轮固定不缓存 (策略 `None`), 要缓存请由调用方按结果里的 `identity` 自理;
/// - 没有核心实现: "core 层"为空, 没人给值就是 `no_impl`。
class JsonProvidePoint : public PointBase {
public:

    JsonProvidePoint(std::string id, PointOptions opts, std::string origin);

    /// 应用侧取值
    asio::awaitable<JsonResult> ask(std::string argsJson, AskOptions opts = {});

    /// 外部主动调用 (插件 / FFI 宿主 / 命令行入口都走这里)
    asio::awaitable<JsonResult> call(std::string argsJson, CallOptions opts = {});

protected:

    asio::awaitable<ChainOutcome> runChain(const ChainEnv& env) override;
};

} // namespace feature
} // namespace agentxx
