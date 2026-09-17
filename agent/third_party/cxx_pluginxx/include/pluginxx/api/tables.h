/// pluginxx 通用接口表 (与宿主领域无关的跨边界 C ABI 表)
///
/// 收录: 事件 / 能力 / 任务调度 / 协程驱动 / 插件互查 / 宿主配置 / 会话取消状态 /
/// JSON 辅助 / 日志 / 后台任务。
///
/// 归属判据: **只需要 JSON 载荷与通用资源即可工作** 的表属于通用表 (本头);
/// 需要宿主领域语义 (工具/权限/钩子/会话/模型/提示词/资源/执行图, 以及 client
/// 侧 UI/事件/会话/线路) 的表由宿主定义与实现。
///
/// 契约稳定性: 全部 C 名、结构体名、IID 字符串冻结 (见 abi.h 说明)。
#ifndef PLUGINXX_API_TABLES_H
#define PLUGINXX_API_TABLES_H

#include "pluginxx/api/abi.h"

#ifdef __cplusplus
extern "C" {
#endif

#pragma pack(push, 8)


/* ==================== 接口表: 事件 (agentxx.agent.events) ==================== */

#define AGENTXX_PLUGIN_IFACE_AGENT_EVENTS         "agentxx.agent.events"
#define AGENTXX_PLUGIN_IFACE_AGENT_EVENTS_VERSION 1

typedef struct AgentxxPluginEventsIface {
    int32_t  version; ///< 必须 == AGENTXX_PLUGIN_IFACE_AGENT_EVENTS_VERSION
    uint32_t struct_size;

    AgentxxPluginSubscription*(AGENTXX_PLUGIN_CALL* subscribe)(
        const AgentxxPluginHost*       host,
        const AgentxxPluginStringView* topic,
        void(AGENTXX_PLUGIN_CALL* handler)(const AgentxxPluginStringView* event_json, void* ud),
        void* ud
    );
    void(AGENTXX_PLUGIN_CALL* unsubscribe)(AgentxxPluginSubscription* sub);
    int32_t(AGENTXX_PLUGIN_CALL* publish)(
        const AgentxxPluginHost*       host,
        const AgentxxPluginStringView* topic,
        const AgentxxPluginStringView* event_json
    );
} AgentxxPluginEventsIface;

/* ==================== 接口表: 能力 (agentxx.agent.capabilities) ==================== */

#define AGENTXX_PLUGIN_IFACE_AGENT_CAPABILITIES         "agentxx.agent.capabilities"
#define AGENTXX_PLUGIN_IFACE_AGENT_CAPABILITIES_VERSION 1

typedef struct AgentxxPluginCapabilitiesIface {
    int32_t  version; ///< 必须 == AGENTXX_PLUGIN_IFACE_AGENT_CAPABILITIES_VERSION
    uint32_t struct_size;

    int32_t(AGENTXX_PLUGIN_CALL* register_capability)(
        const AgentxxPluginHost*       host,
        const AgentxxPluginStringView* capability
    );
    int32_t(AGENTXX_PLUGIN_CALL* register_capability_ex)(
        const AgentxxPluginHost*             host,
        const AgentxxPluginStringView*       capability,
        AgentxxPluginCapabilityStartFunction start,
        AgentxxPluginOperatorCancelFunction  cancel,
        void*                                ctx
    );
    int32_t(AGENTXX_PLUGIN_CALL* unregister_capability)(
        const AgentxxPluginHost*       host,
        const AgentxxPluginStringView* capability
    );
    int32_t(AGENTXX_PLUGIN_CALL* has_capability)(
        const AgentxxPluginHost*       host,
        const AgentxxPluginStringView* capability
    );

    AgentxxPluginOperatorHandle*(AGENTXX_PLUGIN_CALL* invoke_capability_async)(
        const AgentxxPluginHost*       host,
        const AgentxxPluginStringView* capability,
        const AgentxxPluginStringView* method,
        const AgentxxPluginStringView* args_json,
        AgentxxPluginOperatorCallback  cb,
        void*                          ud,
        AgentxxPluginString*           error_out
    );
    void(AGENTXX_PLUGIN_CALL* op_cancel)(AgentxxPluginOperatorHandle* op);
} AgentxxPluginCapabilitiesIface;

/* ==================== 接口表: 任务调度 (agentxx.agent.scheduler) ==================== */

#define AGENTXX_PLUGIN_IFACE_AGENT_SCHEDULER         "agentxx.agent.scheduler"
#define AGENTXX_PLUGIN_IFACE_AGENT_SCHEDULER_VERSION 1

typedef struct AgentxxPluginSchedulerIface {
    int32_t  version; ///< 必须 == AGENTXX_PLUGIN_IFACE_AGENT_SCHEDULER_VERSION
    uint32_t struct_size;

    int32_t(AGENTXX_PLUGIN_CALL* is_io_thread)(const AgentxxPluginHost* host);
    int32_t(AGENTXX_PLUGIN_CALL* post_to_io)(
        const AgentxxPluginHost* host,
        void(AGENTXX_PLUGIN_CALL* fn)(void* ud),
        void* ud
    );
    AgentxxPluginOperatorHandle*(AGENTXX_PLUGIN_CALL* sleep)(
        const AgentxxPluginHost*      host,
        int64_t                       ms,
        AgentxxPluginOperatorCallback cb,
        void*                         ud,
        AgentxxPluginString*          error_out
    );
    void(AGENTXX_PLUGIN_CALL* op_cancel)(AgentxxPluginOperatorHandle* op);

    AgentxxPluginOperatorHandle*(AGENTXX_PLUGIN_CALL* offload)(
        const AgentxxPluginHost* host,
        void*(AGENTXX_PLUGIN_CALL* work)(
            void*                           ud,
            const AgentxxPluginCancelToken* token,
            AgentxxPluginString*            error_out
        ),
        void(AGENTXX_PLUGIN_CALL* done)(
            void*                          ud,
            int32_t                        status,
            void*                          result,
            const AgentxxPluginStringView* error
        ),
        void*                ud,
        AgentxxPluginString* error_out
    );
} AgentxxPluginSchedulerIface;

/* ==================== 接口表: 协程驱动 (agentxx.agent.coroutine_runtime) ==================== */

/// 通用协程驱动接口 (与协程库无关)
///
/// 定位: 插件协程与宿主协程在**同一宿主 IO 执行序列**中交错推进的基础设施。
/// 核心只有两类动作:
/// - **driver/pump**: 插件申请宿主异步执行一次有界回调 (push 一个有限步骤);
/// - **wake 合并**: 插件本地有新工作时自行合并重复请求, 再申请下一次 ticket。
///
/// 关键约束 (宿主与插件共同遵守):
/// - `request_driver` **永不内联**回调, 即使调用者就在宿主 IO 线程; 否则 root start /
///   completion / cancel 会形成意外重入, 并失去交错执行的公平性;
/// - 一次 ticket 至多执行一次回调, 且回调只推进一个有限步骤 (不阻塞、不等待);
/// - 宿主不得把插件私有 reactor 的内部等待对象接进自己的执行序列; 插件必须保证
///   每个 driver 都对应"已知的、真实存在的可运行工作" (外部完成回调 / 定时器回调 /
///   已 post 的 continuation), 不得在无工作时持续申请 ticket (那是隐藏轮询);
/// - 重复 wake 由插件适配器自行合并 (同一实例同时只登记一次 ticket); 宿主另做
///   ticket 去重与关闭时取消作为最后防线。
#define AGENTXX_PLUGIN_IFACE_COROUTINE_RUNTIME         "agentxx.agent.coroutine_runtime"
#define AGENTXX_PLUGIN_IFACE_COROUTINE_RUNTIME_VERSION 1

typedef struct AgentxxPluginCoroutineRuntimeIface {
    int32_t  version; ///< 必须 == AGENTXX_PLUGIN_IFACE_COROUTINE_RUNTIME_VERSION
    uint32_t struct_size;

    /// 申请一次驱动请求 (**任意线程可调用**, 非阻塞):
    /// - 成功返回宿主托管的 ticket (宿主只会**异步**调用 drive_once, 每张至多一次);
    /// - 失败返回 NULL 并在 error_out 输出原因 (host->alloc 分配; 实例已关闭/已停用,
    ///   或宿主无可用 IO executor);
    /// - 失败时调用方必须把受影响的操作以失败/取消终结, 不得静默丢弃。
    AgentxxPluginDriver*(AGENTXX_PLUGIN_CALL* request_driver)(
        const AgentxxPluginHost* host,
        AgentxxPluginDriveOnceFn drive_once,
        void*                    user_data,
        AgentxxPluginString*     error_out
    );

    /// 取消尚未开始的 ticket (**幂等、非阻塞**; 任意线程可调用):
    /// - 尚未执行的 ticket 之后不再执行回调; 正在执行的回调不会被强行中断,
    ///   它由插件自己的 root 收束协议 (取消/完成) 收尾;
    /// - 取消后票不再持有实例 lease, 因此关闭等待可以继续推进。
    void(AGENTXX_PLUGIN_CALL* cancel_driver)(AgentxxPluginDriver* driver);

    /// 当前线程是否为该实例的 IO 线程 (**仅用于断言与诊断**):
    /// - 只允许插件据此检查自己的用法, **不允许**据此内联执行 driver 回调。
    int32_t(AGENTXX_PLUGIN_CALL* is_io_thread)(const AgentxxPluginHost* host);
} AgentxxPluginCoroutineRuntimeIface;

/* ==================== 接口表: 插件互查 (agentxx.agent.plugins) ==================== */

#define AGENTXX_PLUGIN_IFACE_AGENT_PLUGINS         "agentxx.agent.plugins"
#define AGENTXX_PLUGIN_IFACE_AGENT_PLUGINS_VERSION 1

typedef struct AgentxxPluginsIface {
    int32_t  version; ///< 必须 == AGENTXX_PLUGIN_IFACE_AGENT_PLUGINS_VERSION
    uint32_t struct_size;

    int32_t(AGENTXX_PLUGIN_CALL* list_plugins)(
        const AgentxxPluginHost* host,
        AgentxxPluginString*     out
    );
    int32_t(AGENTXX_PLUGIN_CALL* get_plugin)(
        const AgentxxPluginHost*       host,
        const AgentxxPluginStringView* name,
        AgentxxPluginString*           out
    );
    int32_t(AGENTXX_PLUGIN_CALL* get_own_info)(
        const AgentxxPluginHost* host,
        AgentxxPluginString*     out
    );
} AgentxxPluginsIface;

/* ==================== 接口表: 宿主配置 (agentxx.agent.config) ==================== */

#define AGENTXX_PLUGIN_IFACE_AGENT_CONFIG         "agentxx.agent.config"
#define AGENTXX_PLUGIN_IFACE_AGENT_CONFIG_VERSION 1

typedef struct AgentxxPluginConfigIface {
    int32_t  version; ///< 必须 == AGENTXX_PLUGIN_IFACE_AGENT_CONFIG_VERSION
    uint32_t struct_size;

    /// 宿主 AgentConfig 关键字段 JSON (io 线程; host->alloc):
    /// {"dataDir": "...", "projectRoot": "..."(可为空), "platform":
    /// "windows"|"linux"|"macos"|"android"|"ios"}
    int32_t(AGENTXX_PLUGIN_CALL* get_config)(
        const AgentxxPluginHost* host,
        AgentxxPluginString*     out
    );
    /// 本插件配置参数 JSON (yaml `plugins` 条目 args; io 线程; host->alloc):
    int32_t(AGENTXX_PLUGIN_CALL* get_plugin_args)(
        const AgentxxPluginHost* host,
        AgentxxPluginString*     out
    );
    /// 宿主 toolPrompt 配置 (io 线程; host->alloc):
    /// {"depict": "...", "args": {"参数名": "参数说明", ...}}
    int32_t(AGENTXX_PLUGIN_CALL* get_tool_prompt)(
        const AgentxxPluginHost*       host,
        const AgentxxPluginStringView* tool_name,
        AgentxxPluginString*           out
    );
    /// 指定会话生效的工作目录 (io 线程; host->alloc; 失败/未装配返回空串):
    /// - session_id 非空: worktree 绑定优先, 依次回退会话覆写 / AgentConfig
    /// - session_id 为空: 返回解析后的默认会话工作目录
    int32_t(AGENTXX_PLUGIN_CALL* get_session_work_dir)(
        const AgentxxPluginHost*       host,
        const AgentxxPluginStringView* session_id,
        AgentxxPluginString*           out
    );
    /// 本插件配置文件所在目录或文件路径 (yaml `plugins` 条目 config; io 线程;
    /// host->alloc; 未指定返回空串, 空串表示未配置)
    /// - 可指向文件或目录 (由插件自行判断类型并加载)
    /// - 宿主已归一化为绝对路径 (正斜杠, lexically_normal)
    int32_t(AGENTXX_PLUGIN_CALL* get_plugin_config_path)(
        const AgentxxPluginHost* host,
        AgentxxPluginString*     out
    );
    /// 读取当前使用的语言 (io 线程; 返回 0 成功, out 填入语言代码如 "en" / "zh-cn", host->alloc
    /// 分配; 默认 "en")
    int32_t(AGENTXX_PLUGIN_CALL* get_language)(
        const AgentxxPluginHost* host,
        AgentxxPluginString*     out
    );
    /// 指定使用的语言 (io 线程; 返回 0 成功; 不支持 auto, 为空或 auto 时回退为 "en")
    int32_t(AGENTXX_PLUGIN_CALL* set_language)(
        const AgentxxPluginHost*       host,
        const AgentxxPluginStringView* language
    );
} AgentxxPluginConfigIface;

/* ==================== 接口表: 会话取消状态 (agentxx.agent.cancel) ==================== */

#define AGENTXX_PLUGIN_IFACE_AGENT_CANCEL         "agentxx.agent.cancel"
#define AGENTXX_PLUGIN_IFACE_AGENT_CANCEL_VERSION 1

typedef struct AgentxxPluginCancelIface {
    int32_t  version; ///< 必须 == AGENTXX_PLUGIN_IFACE_AGENT_CANCEL_VERSION
    uint32_t struct_size;
    /// 查询会话当前轮次是否已取消 (advisory 定位; 权威通知始终是 cancel 回调)

    int32_t(AGENTXX_PLUGIN_CALL* is_cancelled)(
        const AgentxxPluginHost*       host,
        const AgentxxPluginStringView* session_id
    );
} AgentxxPluginCancelIface;

/* ==================== 接口表: JSON 辅助 (agentxx.agent.json) ==================== */

#define AGENTXX_PLUGIN_IFACE_AGENT_JSON         "agentxx.agent.json"
#define AGENTXX_PLUGIN_IFACE_AGENT_JSON_VERSION 1

typedef struct AgentxxPluginJsonIface {
    int32_t  version; ///< 必须 == AGENTXX_PLUGIN_IFACE_AGENT_JSON_VERSION
    uint32_t struct_size;

    int32_t(AGENTXX_PLUGIN_CALL* json_get_string)(
        const AgentxxPluginHost*       host,
        const AgentxxPluginStringView* json,
        const AgentxxPluginStringView* key,
        AgentxxPluginString*           out
    );
    int32_t(AGENTXX_PLUGIN_CALL* json_escape)(
        const AgentxxPluginHost*       host,
        const AgentxxPluginStringView* s,
        AgentxxPluginString*           out
    );
} AgentxxPluginJsonIface;

/* ==================== 接口表: 日志 (agentxx.agent.log) ==================== */

#define AGENTXX_PLUGIN_IFACE_AGENT_LOG         "agentxx.agent.log"
#define AGENTXX_PLUGIN_IFACE_AGENT_LOG_VERSION 1

typedef struct AgentxxPluginLogIface {
    int32_t  version; ///< 必须 == AGENTXX_PLUGIN_IFACE_AGENT_LOG_VERSION
    uint32_t struct_size;

    void(AGENTXX_PLUGIN_CALL*
             log)(const AgentxxPluginHost* host, int32_t level, const AgentxxPluginStringView* msg);
} AgentxxPluginLogIface;

/* ==================== 接口表: 后台任务 (agentxx.agent.tasks) ==================== */

#define AGENTXX_PLUGIN_IFACE_AGENT_TASKS         "agentxx.agent.tasks"
#define AGENTXX_PLUGIN_IFACE_AGENT_TASKS_VERSION 1

typedef struct AgentxxPluginTasksIface {
    int32_t  version; ///< 必须 == AGENTXX_PLUGIN_IFACE_AGENT_TASKS_VERSION
    uint32_t struct_size;

    /// 注册后台任务 (io 线程约束, 非 io 线程由宿主投递同步等待)。宿主记录
    /// 句柄 (可取消/跟踪完成/持 inflight), 插件协程最终结束时经 *notify
    /// 上报 (恰好一次) → 宿主回收句柄。
    /// - cancel_fn/cancel_ud: 宿主卸载取消时回调 (宿主 io 线程, 协作式):
    ///   唤醒并停止任务; 不可取消可传 NULL
    /// - notify: 【出参】宿主填写的完成通知器 (AgentxxPluginOperatorNotify 值
    ///   拷贝); 插件协程结束 (帧销毁后) 经 notify.done 恰好一次上报 → 宿主
    ///   guard.reset + 回收句柄。以 const 指针形式入参无法回填 —— 宿主只能
    ///   自建一个无法告知插件的 notify, 与本表"插件上报完成"语义矛盾, 必须
    ///   为出参
    /// - notify.done 线程属性与既有 ABI 契约一致: 可从【任意线程】回调
    ///   (宿主 OpCore::onDone 内部原子 CAS + 投递回 io, 线程安全) —— spawn
    ///   协程内若直接调用宿主回调形接口 (invoke_capability_async 等) 或经
    ///   自管线程收尾, 上报可能非 io 线程, 宿主必须按任意线程实现
    /// - 返回宿主托管句柄 (失败返回 NULL 并 *error_out 输出错误, host->alloc)
    AgentxxPluginOperatorHandle*(AGENTXX_PLUGIN_CALL* register_task)(
        const AgentxxPluginHost*            host,
        AgentxxPluginOperatorCancelFunction cancel_fn,
        void*                               cancel_ud,
        AgentxxPluginOperatorNotify*        notify,
        AgentxxPluginString*                error_out
    );
    /// 取消任务 (幂等; 仅限 io 线程调用, 或宿主内部经 ioCallSync 投递后调用)
    /// - 与宿主 detachAll 内部路径一致; 句柄由宿主托管, 跨线程主动取消需经
    ///   scheduler.post_to_io / ioCallSync 回到 io 线程 (与注册类接口线程
    ///   约束一致), 避免 handle->caller 裸指针跨线程反查实例
    void(AGENTXX_PLUGIN_CALL* cancel_task)(AgentxxPluginOperatorHandle* h);
} AgentxxPluginTasksIface;

#pragma pack(pop)

#ifdef __cplusplus
}
#endif

#endif /* PLUGINXX_API_TABLES_H */
