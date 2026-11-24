/// AgentClient —— libagentxx C API (v4: 提交 + 回执) 的 Dart 面向对象封装。
///
/// 线程/并发模型 (与 C++ FFI 层契约对应):
/// - 事件经 `NativeCallable.listener` 接收: 回调在宿主 isolate 上执行, payload 是库分配的一块堆内存
///   (结构体 + 正文), 归回调所有 —— 取完正文后 `agentxx_ffi_free(payload)` 释放整块。
///   因为结构体本身也在那块内存上, 回调延后执行不会悬垂
/// - **所有业务操作都是 `call(op, args)`**: 提交即返回 reqId, 结果经 `EVT_RESULT`
///   送到宿主; 本类维护 `reqId → Completer` 表, 把回执还原成 `Future`
/// - 调用线程上没有任何跨线程等待: 回调里也可以继续调 [call] / [cancelCall]
/// - 同一句柄可以同时有任意多个未完成调用; 需要"前一步生效再发下一步"就
///   `await` 前一步的 Future
/// - `stop()` 受理即返回, 收尾完成后 `EVT_STOPPED` 到达 (本类以 [stopped] 暴露);
///   `destroy` 是纯释放, 只在 Created / Stopped 成功 —— 所以 [dispose] 一定
///   先等 `EVT_STOPPED` 再 destroy
library;

import 'dart:async';
import 'dart:convert';
import 'dart:ffi';

import 'package:agentxx_ffi_bindings/agentxx_ffi_bindings.dart' as bind;
import 'package:ffi/ffi.dart';

import 'dll_loader.dart';
import 'events.dart';

/// 原生调用失败异常 (携带错误码、稳定名字与可选的详情)
class AgentxxException implements Exception {
  const AgentxxException(this.code, [this.name = 'failed', this.detail = '']);

  /// `AGENTXX_FFI_ERR_*` (同步失败) 或回执里的 `error.code`
  final int code;

  /// 稳定名字: `invalid_argument` / `timeout` / `canceled` / `rejected` ...
  final String name;
  final String detail;

  bool get isCanceled => name == 'canceled' || code == bind.AGENTXX_FFI_ERR_CANCELED;
  bool get isTimeout => name == 'timeout' || code == bind.AGENTXX_FFI_ERR_TIMEOUT;

  @override
  String toString() =>
      'AgentxxException($code $name${detail.isEmpty ? '' : ': $detail'})';
}

/// 本次调用被取消 (由 [AgentxxCancelToken] 或 `call_cancel` 触发)
class AgentxxCanceledException extends AgentxxException {
  const AgentxxCanceledException([String detail = 'canceled by caller'])
      : super(bind.AGENTXX_FFI_ERR_CANCELED, 'canceled', detail);
}

/// 取消令牌: 绑到一次或多次 [AgentClient.call] 上, [cancel] 时把它们全部取消
///
/// - 只停止"这次等待", 不撤回已经提交给服务端的操作 (要撤回输入用 `input.cancel`)
/// - 取消后每个调用立刻以 `canceled` 收尾 (迟到应答被丢弃, 不会重复)
class AgentxxCancelToken {
  bool _canceled = false;
  final Map<int, void Function()> _hooks = <int, void Function()>{};

  bool get isCanceled => _canceled;

  /// 取消当前挂在该令牌上的全部调用
  void cancel() {
    _canceled = true;
    final hooks = _hooks.values.toList(growable: false);
    _hooks.clear();
    for (final hook in hooks) {
      hook();
    }
  }

  /// 登记一个 reqId (内部使用; 令牌已取消时立刻执行 [hook])
  void attach(int reqId, void Function() hook) {
    if (_canceled) {
      hook();
      return;
    }
    _hooks[reqId] = hook;
  }

  /// 撤销登记 (调用完成时内部使用)
  void detach(int reqId) {
    _hooks.remove(reqId);
  }
}

/// agent 会话运行时客户端 (单运行时句柄封装)
class AgentClient {
  AgentClient({String? dllPath, DynamicLibrary? lib})
      : _lib = lib ?? openAgentxxLibrary(override: dllPath) {
    _bind = bind.AgentxxFfiBindings(_lib);

    // 版本契约校验: 动态库必须满足最低版本 (>= AGENTXX_FFI_API_VERSION)
    final apiVersion = _bind.agentxx_ffi_api_version();
    if (apiVersion < bind.AGENTXX_FFI_API_VERSION) {
      throw StateError(
        'FFI API 版本不匹配: 动态库=$apiVersion, '
        '绑定期望>=${bind.AGENTXX_FFI_API_VERSION}; 请重新生成绑定或更新动态库',
      );
    }
  }

  final DynamicLibrary _lib;
  late final bind.AgentxxFfiBindings _bind;

  /// 运行时句柄
  Pointer<bind.AgentxxFFIAgent> _handle = nullptr;

  /// 事件回调 (listener: 回调在宿主 isolate 上执行)
  NativeCallable<Void Function(Int32, Pointer<bind.AgentxxString>, Pointer<Void>)>?
      _callable;

  final _eventsController = StreamController<AgentEvent>.broadcast();
  final _readyCompleter = Completer<void>();
  final _stoppedCompleter = Completer<void>();

  /// reqId → 回执等待者
  final Map<int, Completer<ResultEvent>> _pending =
      <int, Completer<ResultEvent>>{};

  bool _started = false;
  bool _stopRequested = false;
  bool _disposed = false;

  /// 解析后的 agent 事件流 (广播; 多订阅者互不影响)
  Stream<AgentEvent> get events => _eventsController.stream;

  /// 首次就绪 (EVT_READY) 的 Future
  Future<void> get ready => _readyCompleter.future;

  /// 收尾完成 (EVT_STOPPED) 的 Future; 未 stop 时永不完成
  Future<void> get stopped => _stoppedCompleter.future;

  /// 当前会话 sessionId (READY 后有效)
  String? sessionId;

  /// 最近一次 `model_info` 给的当前模型名 (回执或广播)
  String? currentModel;

  /// 最近一次权限状态里的"完全授权"标记
  bool fullAuth = false;

  // -------------------------------------------------------------------------
  // 生命周期
  // -------------------------------------------------------------------------

  /// 创建并启动 agent; 返回后事件经 [events] 投递, 就绪用 [ready] 等。
  ///
  /// - [configJson] / [modelJson]: 契约见 `agentxx/ffi_api.h` 的 create
  /// - [syncTailCount] 首屏只同步最近 N 条展示消息 (`0` = 完整同步);
  ///   更早的历史用 [viewPage] 按页拉。该值写进 `config_json` 一起交给 create
  Future<void> start({
    String? configJson,
    String? modelJson,
    int syncTailCount = 200,
  }) async {
    if (_started || _handle != nullptr) {
      throw StateError('AgentClient 已启动');
    }
    if (!_eventsController.hasListener) {
      throw StateError('请先订阅 events 流再 start (否则会丢失 READY 前事件)');
    }

    _callable ??= NativeCallable<
        Void Function(Int32, Pointer<bind.AgentxxString>, Pointer<Void>)>.listener(
      _onNativeEvent,
    );

    final cfg = _mergeConfig(configJson, <String, Object?>{
      'syncTailCount': syncTailCount,
    });

    final cb = calloc<bind.AgentxxFFICallbacks>();
    try {
      cb.ref.struct_size = sizeOf<bind.AgentxxFFICallbacks>();
      cb.ref.reserved = 0;
      cb.ref.on_event = _callable!.nativeFunction;
      cb.ref.user_data = nullptr;

      // 字符串参数由原生侧在 create 内同步拷贝, 临时 UTF-8 缓冲随即释放
      final created = _Utf8View.withAll([cfg, modelJson], (views) {
        return _withLog(
          (logPtr) => _bind.agentxx_ffi_create(
              views[0], views[1], cb, logPtr),
        );
      });
      final (handle, log) = created;
      if (handle == nullptr) {
        throw AgentxxException(
          bind.AGENTXX_FFI_ERR_INIT,
          'init_failed',
          log ?? 'create 失败 (未返回句柄)',
        );
      }
      _handle = handle;
    } finally {
      calloc.free(cb);
    }

    final (startRc, startLog) =
        _withLog((logPtr) => _bind.agentxx_ffi_start(_handle, logPtr));
    if (startRc != bind.AGENTXX_FFI_OK) {
      final handle = _handle;
      _handle = nullptr;
      if (handle != nullptr) {
        _withLog((logPtr) => _bind.agentxx_ffi_destroy(handle, logPtr));
      }
      throw AgentxxException(startRc, 'start_failed', startLog ?? 'start 受理失败');
    }
    _started = true;
  }

  /// 停止: 受理即返回, 收尾完成后 [stopped] 完成 (幂等)
  Future<void> stop() async {
    if (_handle == nullptr) {
      return;
    }
    if (!_stopRequested) {
      _stopRequested = true;
      final (rc, log) =
          _withLog((logPtr) => _bind.agentxx_ffi_stop(_handle, logPtr));
      if (rc != bind.AGENTXX_FFI_OK) {
        throw AgentxxException(rc, 'stop_failed', log ?? 'stop 受理失败');
      }
    }
    await _stoppedCompleter.future;
  }

  /// 停止 + 销毁 + 释放回调与事件流 (幂等; 未启动时安全调用)
  Future<void> dispose() async {
    if (_disposed) {
      return;
    }
    _disposed = true;
    try {
      await stop();
    } catch (_) {
      // stop 失败也要继续释放: 不吞掉"资源回收", 只丢掉这次错误
    }
    final handle = _handle;
    _handle = nullptr;
    if (handle != nullptr) {
      final (_, log) =
          _withLog((logPtr) => _bind.agentxx_ffi_destroy(handle, logPtr));
      if (log != null) {
        // destroy 只在 Created / Stopped 成功; 失败说明收尾没走完
        throw AgentxxException(
            bind.AGENTXX_FFI_ERR_STATE, 'destroy_failed', log);
      }
    }
    _callable?.close();
    _callable = null;
    _failPending(const AgentxxCanceledException('client disposed'));
    if (!_eventsController.isClosed) {
      await _eventsController.close();
    }
  }

  // -------------------------------------------------------------------------
  // 调用 (唯一的业务入口)
  // -------------------------------------------------------------------------

  /// 提交一次调用并等回执
  ///
  /// - [op] 见 `ffi.md` 的 op 表 (也可用 [capabilities] 的 `ops` 取全集)
  /// - [args] 按 op 约定; 会话作用域由原生侧自动填, 不用写 sessionId
  /// - [timeout] 只覆盖本次调用的服务端时限 (`0` = 不限; 缺省用 op 默认值)
  /// - [cancel] 取消令牌: 令牌取消时本次调用以 [AgentxxCanceledException] 结束
  ///
  /// 失败抛 [AgentxxException] (`error.name` 是稳定字符串); 成功返回 `result`
  /// 对象 (无结果的 op 为空对象)。
  Future<Map<String, dynamic>> call(
    String op, {
    Map<String, Object?> args = const <String, Object?>{},
    Duration? timeout,
    AgentxxCancelToken? cancel,
  }) async {
    final reqId = _submit(op, args, timeout);
    final completer = Completer<ResultEvent>();
    _pending[reqId] = completer;
    cancel?.attach(reqId, () => _cancelQuietly(reqId));

    final ResultEvent result;
    try {
      // 原生侧到点会自己回一条 timeout 回执; 这里的兵底只防"回执彻底丢失"
      result = await (timeout == null
          ? completer.future
          : completer.future.timeout(
              timeout + const Duration(seconds: 10),
              onTimeout: () {
                _cancelQuietly(reqId);
                throw AgentxxException(
                  bind.AGENTXX_FFI_ERR_TIMEOUT,
                  'timeout',
                  '$op 未在 ${timeout.inMilliseconds} ms 内拿到回执',
                );
              },
            ));
    } finally {
      _pending.remove(reqId);
      cancel?.detach(reqId);
    }

    if (!result.ok) {
      final err = result.error;
      // 取消是"正常的提前结束": 给一个可单独 catch 的子类, 调用方不必自己比错误名
      if (err != null
          && (err.name == 'canceled' || err.code == bind.AGENTXX_FFI_ERR_CANCELED)) {
        throw AgentxxCanceledException(
            err.message.isEmpty ? 'canceled by caller' : err.message);
      }
      throw AgentxxException(
        err?.code ?? bind.AGENTXX_FFI_ERR_REJECTED,
        err?.name ?? 'rejected',
        err?.message ?? '',
      );
    }
    return result.result;
  }

  /// 只提交不等回执; 返回 reqId (回执仍会经 [events] 的 [ResultEvent] 到达)
  int callNoWait(
    String op, {
    Map<String, Object?> args = const <String, Object?>{},
    Duration? timeout,
  }) =>
      _submit(op, args, timeout);

  /// 取消一次仍在等待的调用 (幂等: 已完成时静默返回)
  void cancelCall(int reqId) {
    if (_handle == nullptr) {
      return;
    }
    _withLog((logPtr) => _bind.agentxx_ffi_call_cancel(_handle, reqId, logPtr));
  }

  /// 能力清单: `{apiVersion, libraryVersion, ops[], features[]}`
  Future<Map<String, dynamic>> capabilities() async {
    if (_handle == nullptr) {
      throw const AgentxxException(bind.AGENTXX_FFI_ERR_STATE, 'not_started');
    }
    final out = calloc<bind.AgentxxString>();
    try {
      final (rc, log) =
          _withLog((logPtr) => _bind.agentxx_ffi_get_capabilities(_handle, out, logPtr));
      if (rc != bind.AGENTXX_FFI_OK) {
        throw AgentxxException(rc, 'capabilities_failed', log ?? '');
      }
      final text = _takeOwned(out) ?? '{}';
      final decoded = jsonDecode(text);
      return decoded is Map<String, dynamic> ? decoded : <String, dynamic>{};
    } finally {
      calloc.free(out);
    }
  }

  /// 取走积压日志 (排障用): `[{"level","message"},...]`
  List<dynamic>? drainLogs() {
    if (_handle == nullptr) {
      return null;
    }
    final out = calloc<bind.AgentxxString>();
    try {
      final (rc, _) =
          _withLog((logPtr) => _bind.agentxx_ffi_drain_logs(_handle, out, logPtr));
      if (rc != bind.AGENTXX_FFI_OK) {
        return null;
      }
      final text = _takeOwned(out) ?? '';
      if (text.isEmpty) {
        return const <dynamic>[];
      }
      final v = jsonDecode(text);
      return v is List ? v : null;
    } on FormatException {
      return null;
    } finally {
      calloc.free(out);
    }
  }

  // -------------------------------------------------------------------------
  // 便捷操作 (全部是 await 的 Future; 失败抛 AgentxxException)
  // -------------------------------------------------------------------------

  /// 发送用户输入 (文本与附件至少一个); 结果含 `{status, delivery, itemId?}`
  Future<Map<String, dynamic>> sendInput(
    String text, {
    List<Map<String, Object?>> attachments = const [],
    String? model,
    String? delivery,
  }) {
    return call('input.send', args: <String, Object?>{
      'text': text,
      if (attachments.isNotEmpty) 'attachments': attachments,
      if (model != null && model.isNotEmpty) 'model': model,
      if (delivery != null && delivery.isNotEmpty) 'delivery': delivery,
    });
  }

  /// 取消当前轮次 (只置取消标记, 不撤回已提交的输入)
  Future<Map<String, dynamic>> cancelTurn() => call('input.cancel');

  /// 清空待发队列
  Future<Map<String, dynamic>> clearQueue() => call('input.queue.clear');

  /// 从待发队列移除一条 ([itemId] 来自消息队列事件)
  Future<Map<String, dynamic>> removeQueued(String itemId) =>
      call('input.queue.remove', args: <String, Object?>{'itemId': itemId});

  /// 中断当前轮次并立刻开始队列里的下一条
  Future<Map<String, dynamic>> runNext() => call('input.queue.run_next');

  /// 模型状态: `{currentModel, models[], capabilities[]}`
  Future<Map<String, dynamic>> listModels() async {
    final result = await call('model.list');
    final model = result['currentModel'];
    if (model is String && model.isNotEmpty) {
      currentModel = model;
    }
    return result;
  }

  /// 切换模型 (空名字 = 沿用当前); 回执 data 给出生效后的模型名
  Future<Map<String, dynamic>> selectModel(String name) async {
    final result = await call('model.select', args: <String, Object?>{'name': name});
    final model = result['model'];
    if (model is String && model.isNotEmpty) {
      currentModel = model;
    }
    return result;
  }

  /// 新增模型配置 (与 `create` 的 modelJson 同字段); 需要写配置落盘
  Future<Map<String, dynamic>> addModel(Map<String, Object?> modelConfig) =>
      call('model.add', args: modelConfig, timeout: const Duration(seconds: 30));

  /// 移除模型配置 (删除当前模型会被拒)
  Future<Map<String, dynamic>> removeModel(String name) => call(
        'model.remove',
        args: <String, Object?>{'name': name},
        timeout: const Duration(seconds: 30),
      );

  /// 持久化会话列表: `{sessions[], totalCount, hasMore}`
  Future<Map<String, dynamic>> listSessions({
    int? beforeMs,
    String? beforeId,
    int? limit,
    String? keyword,
  }) {
    return call('session.list', args: <String, Object?>{
      if (beforeMs != null) 'beforeMs': beforeMs,
      if (beforeId != null && beforeId.isNotEmpty) 'beforeId': beforeId,
      if (limit != null) 'limit': limit,
      if (keyword != null && keyword.isNotEmpty) 'keyword': keyword,
    });
  }

  /// 切换会话 (需开启会话持久化); 历史/模型/统计事件先到, 回执在切换提交后到
  Future<Map<String, dynamic>> switchSession(String sid) async {
    final result = await call(
      'session.switch',
      args: <String, Object?>{'sessionId': sid},
      timeout: const Duration(seconds: 60),
    );
    final switched = result['sessionId'];
    sessionId = (switched is String && switched.isNotEmpty) ? switched : sid;
    return result;
  }

  /// 重命名当前会话
  Future<Map<String, dynamic>> renameSession(String title) =>
      call('session.rename', args: <String, Object?>{'title': title});

  /// 压缩当前会话上下文 (可能跑很久: 回执在压缩真的结束后才到)
  Future<Map<String, dynamic>> compact() => call('session.compact');

  /// LLM 上下文消息: `{messages[]}` ([tail] 只取最近 N 条)
  Future<Map<String, dynamic>> getContext({int? tail}) => call(
        'context.get',
        args: <String, Object?>{if (tail != null) 'tail': tail},
      );

  /// 展示历史分页: `{startIndex, totalCount, messages[]}`
  Future<Map<String, dynamic>> viewPage({int? beforeIndex, int? count}) {
    return call('context.view_page', args: <String, Object?>{
      if (beforeIndex != null) 'beforeIndex': beforeIndex,
      if (count != null) 'count': count,
    });
  }

  /// 启动组件信息 (MCP/Skill/Memory/插件): `{notifications[]}`
  Future<Map<String, dynamic>> getComponents() => call('components.get');

  /// 权限状态: `{fullAuth}`
  Future<Map<String, dynamic>> getPermissionState() async {
    final result = await call('permission.get_state');
    final flag = result['fullAuth'];
    if (flag is bool) {
      fullAuth = flag;
    }
    return result;
  }

  /// 切换"完全授权"; 回执里是生效后的状态
  Future<Map<String, dynamic>> setFullAuth(bool value) async {
    final result = await call(
      'permission.set_full_auth',
      args: <String, Object?>{'fullAuth': value},
    );
    final flag = result['fullAuth'];
    if (flag is bool) {
      fullAuth = flag;
    }
    return result;
  }

  /// 列目录 (跨设备附件选择用): `{path, entries[]}` 形态见 wire `list_dir_result`
  Future<Map<String, dynamic>> listDir(
    String path, {
    List<String> allowedExtensions = const [],
  }) {
    return call(
      'fs.list_dir',
      args: <String, Object?>{
        'path': path,
        if (allowedExtensions.isNotEmpty) 'allowedExtensions': allowedExtensions,
      },
      timeout: const Duration(seconds: 30),
    );
  }

  /// 应答 HIL 中断 ([values] 为 `{控件 id: 值}`, 空对象 = 未应答)
  Future<Map<String, dynamic>> respondInterrupt(
    int interruptId,
    Map<String, Object?> values,
  ) {
    return call('interrupt.respond', args: <String, Object?>{
      'interruptId': interruptId,
      'values': values,
    });
  }

  /// 注册宿主程序提供的工具; 回执 `{registered[], failed[{name,error}]}`
  Future<Map<String, dynamic>> registerHostTools(
    List<Map<String, Object?>> tools,
  ) =>
      call('hosttool.register', args: <String, Object?>{'tools': tools});

  /// 注销宿主工具; 回执 `{removed[]}`
  Future<Map<String, dynamic>> unregisterHostTools(List<String> names) =>
      call('hosttool.unregister', args: <String, Object?>{'names': names});

  /// 回应宿主工具调用 (callId 来自 [HostToolCallEvent])
  Future<Map<String, dynamic>> respondHostTool(
    int callId, {
    bool ok = true,
    Object? result,
    Object? error,
  }) {
    return call('hosttool.respond', args: <String, Object?>{
      'callId': callId,
      'ok': ok,
      if (ok && result != null) 'result': result,
      if (!ok && error != null) 'error': error,
    });
  }

  /// 把插件数据发布到 agent 总线 (topic 必须在 `agentxx.client.*` / `plugin.*` 等族内)
  Future<Map<String, dynamic>> publishPluginData(String topic, Object? data) {
    return call('plugin.data_up', args: <String, Object?>{
      'topic': topic,
      if (data != null) 'data': data,
    });
  }

  /// 会话语言 (`en` / `zh-cn` ...)
  Future<Map<String, dynamic>> getLanguage() => call('language.get');

  /// 设置会话语言 (返回规范化后的值)
  Future<Map<String, dynamic>> setLanguage(String language) =>
      call('language.set', args: <String, Object?>{'language': language});

  /// 打开增量合批 (窗口 [maxDelayMs], 建议 16~32ms)
  ///
  /// **默认关闭**: 逐 token 事件在跨语言边界上有固定成本, 高频流式输出时打开
  /// 能显著减少回调次数; 代价是文本按窗口成块到达 (需要"回执前增量已到齐"
  /// 这类强顺序时不要打开)。
  Future<Map<String, dynamic>> enableDeltaBatch({int maxDelayMs = 24}) =>
      call('delta.set_batch', args: <String, Object?>{'maxDelayMs': maxDelayMs});

  /// 关闭增量合批 (逐条 `EVT_DELTA_TEXT_BATCH` / `EVT_DELTA` 到达)
  Future<Map<String, dynamic>> disableDeltaBatch() =>
      call('delta.set_batch', args: <String, Object?>{'maxDelayMs': 0});

  // -------------------------------------------------------------------------
  // 内部: 提交
  // -------------------------------------------------------------------------

  int _submit(String op, Map<String, Object?> args, Duration? timeout) {
    if (_handle == nullptr) {
      throw const AgentxxException(
          bind.AGENTXX_FFI_ERR_STATE, 'not_started', 'AgentClient 未启动或已销毁');
    }
    final request = jsonEncode(<String, Object?>{
      'op': op,
      'args': args,
      if (timeout != null && timeout.inMilliseconds > 0)
        'timeoutMs': timeout.inMilliseconds,
    });

    final reqIdOut = calloc<Uint64>();
    try {
      final (rc, log) = _Utf8View.withAll([request], (views) {
        return _withLog(
          (logPtr) => _bind.agentxx_ffi_call(_handle, views[0], reqIdOut, logPtr),
        );
      });
      if (rc != bind.AGENTXX_FFI_OK) {
        // 同步失败: 不会有回执, 也不用登记等待
        throw AgentxxException(rc, 'submit_failed', log ?? '$op 提交失败');
      }
      final reqId = reqIdOut.value;
      if (reqId == 0) {
        throw AgentxxException(
            bind.AGENTXX_FFI_ERR_INTERNAL, 'submit_failed', '$op 未返回 reqId');
      }
      return reqId;
    } finally {
      calloc.free(reqIdOut);
    }
  }

  void _cancelQuietly(int reqId) {
    try {
      cancelCall(reqId);
    } catch (_) {
      // 取消失败不影响调用方: 回执仍会到 (或被本地超时兜底)
    }
  }

  void _failPending(AgentxxException error) {
    final waiters = _pending.values.toList(growable: false);
    _pending.clear();
    for (final waiter in waiters) {
      if (!waiter.isCompleted) {
        waiter.completeError(error);
      }
    }
  }

  // -------------------------------------------------------------------------
  // 内部: 事件分发
  // -------------------------------------------------------------------------

  /// 原生事件回调 (运行在宿主 isolate; 任何异常都不能外抛)
  void _onNativeEvent(
    int type,
    Pointer<bind.AgentxxString> payload,
    Pointer<Void> userData,
  ) {
    String text = '';
    try {
      if (payload != nullptr) {
        final s = payload.ref;
        if (s.data != nullptr && s.size > 0) {
          text = utf8.decode(
            s.data.cast<Uint8>().asTypedList(s.size.toInt()),
            allowMalformed: true,
          );
        }
        // payload 归回调所有: 结构体与正文同在一块堆内存上, 取完用 agentxx_ffi_free 释放整块
        _bind.agentxx_ffi_free(payload.cast<Void>());
      }
    } catch (_) {
      return;
    }

    final event = parseAgentEvent(type, text);
    if (event == null) {
      return;
    }
    try {
      _handleEvent(event);
    } catch (_) {
      // 分发里的错误不影响原生线程
    }
    if (!_eventsController.isClosed) {
      _eventsController.add(event);
    }
  }

  void _handleEvent(AgentEvent event) {
    switch (event) {
      case ReadyEvent():
        final sid = event.sessionId;
        if (sid.isNotEmpty) {
          sessionId = sid;
        }
        if (!_readyCompleter.isCompleted) {
          _readyCompleter.complete();
        }
      case StoppedEvent():
        if (!_stoppedCompleter.isCompleted) {
          _stoppedCompleter.complete();
        }
        // 收尾时未完成的调用已由原生侧按 canceled 收尾; 这里只做兜底
        _failPending(const AgentxxCanceledException('runtime stopped'));
      case ResultEvent():
        final waiter = _pending.remove(event.reqId);
        if (waiter != null && !waiter.isCompleted) {
          waiter.complete(event);
        }
      case ModelInfoEvent():
        final model = event.currentModel;
        if (model.isNotEmpty) {
          currentModel = model;
        }
      case PermissionStateEvent():
        fullAuth = event.fullAuth;
      case ErrorEvent():
        // 就绪前的内部错误视为启动失败
        if (!_readyCompleter.isCompleted && _started) {
          _readyCompleter.completeError(
            AgentxxException(event.code, event.name, event.message),
          );
        }
      default:
        break;
    }
  }

  // -------------------------------------------------------------------------
  // 内部: 原生内存辅助
  // -------------------------------------------------------------------------

  /// 把 [configJson] 与覆盖项合并成一份新 JSON (configJson 为空时只用覆盖项)
  static String? _mergeConfig(String? configJson, Map<String, Object?> overrides) {
    Map<String, dynamic> merged = <String, dynamic>{};
    if (configJson != null && configJson.trim().isNotEmpty) {
      final decoded = jsonDecode(configJson);
      if (decoded is! Map<String, dynamic>) {
        throw StateError('config_json 必须是 JSON 对象');
      }
      merged = decoded;
    }
    merged.addAll(overrides);
    return jsonEncode(merged);
  }

  /// 运行 [fn] 并捕获 log 出参文本; 返回 (fn 结果, log 文本或 null)
  (R, String?) _withLog<R>(R Function(Pointer<bind.AgentxxString> logPtr) fn) {
    final logPtr = calloc<bind.AgentxxString>();
    try {
      final r = fn(logPtr);
      return (r, _takeOwned(logPtr));
    } finally {
      calloc.free(logPtr);
    }
  }

  /// 读取并释放由库分配、归调用方所有的 [AgentxxString] (内容为空时返回 null)
  String? _takeOwned(Pointer<bind.AgentxxString> out) {
    final s = out.ref;
    if (s.data == nullptr || s.size == 0) {
      return null;
    }
    final text = utf8.decode(
      s.data.cast<Uint8>().asTypedList(s.size.toInt()),
      allowMalformed: true,
    );
    _bind.agentxx_ffi_string_free(out);
    return text;
  }
}

/// 临时 `AgentxxStringView` 参数视图 (指向自己持有的 UTF-8 缓冲)
///
/// - v4 的 `create` / `call` 收 `const AgentxxStringView*`: 调用方要自己准备
///   结构体, 原生侧只在本次调用内读取
/// - `null` 字符串写成"data == NULL, size == 0" (契约里的"未给"形态)
class _Utf8View {
  _Utf8View._(this._buffer, this._view);

  factory _Utf8View(String? s) {
    final view = calloc<bind.AgentxxStringView>();
    if (s == null) {
      return _Utf8View._(nullptr, view);
    }
    final units = utf8.encode(s);
    final buffer = calloc<Uint8>(units.length + 1);
    if (units.isNotEmpty) {
      buffer.asTypedList(units.length).setAll(0, units);
    }
    buffer[units.length] = 0;
    view.ref.data = buffer.cast<Char>();
    view.ref.size = units.length;
    return _Utf8View._(buffer, view);
  }

  /// 一次性准备多个视图: 回调收到与 [inputs] 顺序一致的指针数组
  static R withAll<R>(
    List<String?> inputs,
    R Function(List<Pointer<bind.AgentxxStringView>> views) body,
  ) {
    final owned = inputs.map(_Utf8View.new).toList(growable: false);
    try {
      return body(owned.map((v) => v._view).toList(growable: false));
    } finally {
      for (final v in owned) {
        v.dispose();
      }
    }
  }

  final Pointer<Uint8> _buffer;
  final Pointer<bind.AgentxxStringView> _view;

  void dispose() {
    if (_buffer != nullptr) {
      calloc.free(_buffer);
    }
    calloc.free(_view);
  }
}
