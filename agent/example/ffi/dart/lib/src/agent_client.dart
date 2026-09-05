/// AgentClient —— libagentxx C API 的 Dart 面向对象封装。
///
/// 线程/并发模型 (与 C++ FFI 层契约对应):
/// - 事件经 "异步安全事件队列桥接" 接收: 创建时把 AgentxxFFICallbacks.on_event
///   指向导出符号 `agentxx_ffi_event_queue_on_event`, user_data 指向队列句柄。
///   原生 io 线程在回调内同步拷贝 payload 入队, 本类随后台轮询循环取出并
///   解析为 [AgentEvent] 广播到 [events] 流 —— 全程无悬垂指针风险
///   (直接用 NativeCallable.listener 会因 "payload 仅回调期间有效" 而悬垂)。
/// - 同步查询 (get_model_info 等) 直接转发原生调用; 返回字符串经 agentxx_ffi_free
///   释放。同一句柄同一时刻仅允许一个尚未返回的同步查询 (见 ffi_api.h 注释)。
/// - stop/destroy 必须在回调线程以外调用 —— Dart 主 isolate 天然满足。
library;

import 'dart:async';
import 'dart:convert';
import 'dart:ffi';

import 'package:agentxx_ffi_bindings/agentxx_ffi_bindings.dart' as bind;
import 'package:ffi/ffi.dart';

import 'dll_loader.dart';
import 'events.dart';

/// 原生调用失败异常 (携带错误码与可选的详情日志)
class AgentxxException implements Exception {
  const AgentxxException(this.code, [this.detail = '']);

  final int code;
  final String detail;

  @override
  String toString() =>
      'AgentxxException($code${detail.isEmpty ? '' : ', $detail'})';
}

/// agent 会话运行时客户端 (单运行时句柄封装)
class AgentClient {
  AgentClient({String? dllPath, DynamicLibrary? lib})
      : _lib = lib ?? openAgentxxLibrary(override: dllPath) {
    _bind = bind.AgentxxFfiBindings(_lib);

    // 版本契约校验: 绑定与动态库必须同代 (AGENTXX_FFI_API_VERSION)
    final apiVersion = _bind.agentxx_ffi_api_version();
    if (apiVersion != bind.AGENTXX_FFI_API_VERSION) {
      throw StateError(
        'FFI API 版本不匹配: 动态库=$apiVersion, '
        '绑定=${bind.AGENTXX_FFI_API_VERSION}; 请重新生成绑定或更新动态库',
      );
    }
  }

  final DynamicLibrary _lib;
  late final bind.AgentxxFfiBindings _bind;

  /// 事件队列句柄 (异步安全桥接; 见文件头注释)
  Pointer<bind.AgentxxFFIEventQueue>? _queue;

  /// 运行时句柄
  Pointer<bind.AgentxxFFIAgent> _handle = nullptr;

  bool _running = false;
  Future<void> _pollFuture = Future.value();

  final _eventsController = StreamController<AgentEvent>.broadcast();
  final _readyCompleter = Completer<void>();

  /// 解析后的 agent 事件流 (广播; 多订阅者互不影响)
  Stream<AgentEvent> get events => _eventsController.stream;

  /// 首次就绪 (EVT_READY) 的 Future
  Future<void> get ready => _readyCompleter.future;

  /// 当前会话 sessionId (READY 后有效)
  String? sessionId;

  // -------------------------------------------------------------------------
  // 生命周期
  // -------------------------------------------------------------------------

  /// 创建并异步启动 agent ([configJson]/[modelJson] 见 ffi_api.h 契约);
  /// 返回后事件经 [events] 流投递, 就绪以 [ready]/EVT_READY 标记。
  ///
  /// 注意: 字符串参数均由原生侧在调用内同步拷贝, 本封装经 [withUtf8] 在
  /// 调用返回后立即释放临时 UTF-8 缓冲。
  Future<void> start({
    String? configJson,
    String? modelJson,
  }) async {
    if (_running || _handle != nullptr) {
      throw StateError('AgentClient 已启动');
    }
    if (!_eventsController.hasListener) {
      throw StateError('请先订阅 events 流再 start (否则会丢失 READY 前事件)');
    }

    _queue = _bind.agentxx_ffi_event_queue_create();
    final q = _queue;
    if (q == null || q == nullptr) {
      throw const AgentxxException(bind.AGENTXX_FFI_ERR_OOM, '创建事件队列失败');
    }

    // 注册事件队列桥接: on_event = 导出符号 agentxx_ffi_event_queue_on_event,
    // 签名与 AgentxxFFICallbacks.on_event 完全一致, user_data = 队列句柄。
    // 原生侧 create 时对 callbacks "值拷贝", 故临时 cb 结构可随即释放。
    final trampoline = _lib.lookup<
        NativeFunction<
            Void Function(UnsignedInt, Pointer<Char>,
                Pointer<Void>)>>('agentxx_ffi_event_queue_on_event');
    final cb = calloc<bind.AgentxxFFICallbacks>();
    try {
      cb.ref.on_event = trampoline;
      cb.ref.user_data = Pointer<Void>.fromAddress(q.address);

      // 空 JSON 传 nullptr (NULL = 使用默认配置; 空串会触发非法 JSON 错误)
      final created = withUtf8(_nullIfEmpty(configJson), (configPtr) {
        return withUtf8(_nullIfEmpty(modelJson), (modelPtr) {
          return _withLog((logPtr) =>
              _bind.agentxx_ffi_create(configPtr, modelPtr, cb, logPtr));
        });
      });
      final (handle, log) = created;
      if (handle == nullptr) {
        // create 失败清理: 队列一并释放, 恢复可重试状态
        _bind.agentxx_ffi_event_queue_free(q);
        _queue = null;
        throw AgentxxException(
          bind.AGENTXX_FFI_ERR_INIT,
          log ?? 'create 失败 (未返回句柄)',
        );
      }
      _handle = handle;
    } finally {
      calloc.free(cb);
    }

    // 异步启动: 立即受理, 就绪经 EVT_READY / 启动失败经 EVT_ERROR 上报
    try {
      final (startRc, startLog) =
          _withLog((logPtr) => _bind.agentxx_ffi_start(_handle, logPtr));
      if (startRc != bind.AGENTXX_FFI_OK) {
        throw AgentxxException(startRc, startLog ?? 'start 受理失败');
      }
    } on Object {
      // 失败清理: 销毁句柄与事件队列, 恢复到可重试的初始状态 (不吞异常)
      if (_handle != nullptr) {
        _bind.agentxx_ffi_destroy(_handle, nullptr);
        _handle = nullptr;
      }
      final q = _queue;
      if (q != null && q != nullptr) {
        _bind.agentxx_ffi_event_queue_free(q);
        _queue = null;
      }
      rethrow;
    }

    // READY 后记录 sessionId (供 switch_session 等使用)
    events.where((e) => e is ReadyEvent).cast<ReadyEvent>().take(1).listen((e) {
      sessionId = e.sessionId;
      if (!_readyCompleter.isCompleted) {
        _readyCompleter.complete();
      }
    });
    events.where((e) => e is ErrorEvent).cast<ErrorEvent>().listen((e) {
      // 就绪前出现的内部错误视为启动失败 (就绪后的错误经正常事件流呈现)
      if (!_readyCompleter.isCompleted && _running) {
        _readyCompleter.completeError(AgentxxException(e.code, e.message));
      }
    });

    _running = true;
    _pollFuture = _pollLoop();
  }

  /// 同步停止并回收 (阻塞至 io 线程退出; 幂等; 未启动时安全调用)
  Future<void> dispose() async {
    _running = false;
    await _pollFuture;

    if (_handle != nullptr) {
      _bind.agentxx_ffi_stop(_handle, nullptr);
      _bind.agentxx_ffi_destroy(_handle, nullptr);
      _handle = nullptr;
    }
    final q = _queue;
    if (q != null && q != nullptr) {
      _bind.agentxx_ffi_event_queue_free(q);
      _queue = null;
    }
    await _eventsController.close();
  }

  // -------------------------------------------------------------------------
  // 会话交互 (转发原生; 失败抛 AgentxxException)
  // -------------------------------------------------------------------------

  /// 发送用户输入 (READY 前发送会被服务端缓存, 就绪后按序处理)
  void sendInput(String text) {
    _checkHandle('发送输入');
    final (rc, log) = withUtf8(
        text,
        (textPtr) => _withLog((logPtr) =>
            _bind.agentxx_ffi_send_input(_handle, textPtr, logPtr)));
    if (rc != bind.AGENTXX_FFI_OK) {
      throw AgentxxException(rc, log ?? '发送输入失败');
    }
  }

  /// 取消当前轮次
  void cancel() {
    _checkHandle('取消轮次');
    final (rc, log) =
        _withLog((logPtr) => _bind.agentxx_ffi_cancel(_handle, logPtr));
    if (rc != bind.AGENTXX_FFI_OK) {
      throw AgentxxException(rc, log ?? '取消当前轮次失败');
    }
  }

  /// 切换模型 (结果经 EVT_MODEL_INFO 通知)
  void selectModel(String modelName) {
    _checkHandle('切换模型');
    final (rc, log) = withUtf8(
        modelName,
        (namePtr) => _withLog((logPtr) =>
            _bind.agentxx_ffi_select_model(_handle, namePtr, logPtr)));
    if (rc != bind.AGENTXX_FFI_OK) {
      throw AgentxxException(rc, log ?? '切换模型失败');
    }
  }

  /// 记住权限选择 ([op]: 0=读取 1=写入; [allow]: 允许/拒绝)
  void setPermission(String path, {required bool allow, required int op}) {
    _checkHandle('记住权限');
    final (rc, log) = withUtf8(
        path,
        (pathPtr) => _withLog((logPtr) => _bind.agentxx_ffi_set_permission(
            _handle, pathPtr, allow ? 1 : 0, op, logPtr)));
    if (rc != bind.AGENTXX_FFI_OK) {
      throw AgentxxException(rc, log ?? '记住权限失败');
    }
  }

  /// 切换会话 (需开启持久化; 结果经 Sync/ModelInfo/ContextStats 事件回推)
  void switchSession(String sid) {
    _checkHandle('切换会话');
    final (rc, log) = withUtf8(
        sid,
        (sidPtr) => _withLog((logPtr) =>
            _bind.agentxx_ffi_switch_session(_handle, sidPtr, logPtr)));
    if (rc != bind.AGENTXX_FFI_OK) {
      throw AgentxxException(rc, log ?? '切换会话失败');
    }
  }

  /// 应答 HIL 中断 ([valuesJson]: 与中断 inputs 顺序一一对应的 JSON 字符串数组)
  void interruptRespond(int interruptId, String valuesJson) {
    _checkHandle('应答中断');
    final (rc, log) = withUtf8(
        valuesJson,
        (valuesPtr) => _withLog((logPtr) => _bind.agentxx_ffi_interrupt_respond(
            _handle, interruptId, valuesPtr, logPtr)));
    if (rc != bind.AGENTXX_FFI_OK) {
      throw AgentxxException(rc, log ?? '应答中断失败');
    }
  }

  // -------------------------------------------------------------------------
  // 同步查询 (返回解析后的 JSON 对象; 失败返回 null 并可查 [lastQueryError])
  // -------------------------------------------------------------------------

  String? lastQueryError;

  Map<String, dynamic>? getModelInfo() =>
      _syncQuery((h, log) => _bind.agentxx_ffi_get_model_info(h, log));

  Map<String, dynamic>? getContextMessages() =>
      _syncQuery((h, log) => _bind.agentxx_ffi_get_context_messages(h, log));

  Map<String, dynamic>? listSessions() =>
      _syncQuery((h, log) => _bind.agentxx_ffi_list_sessions(h, log));

  /// 取走积压日志 (排障用): [{"level","message"},...]
  List<dynamic>? drainLogs() {
    if (_handle == nullptr) {
      return null;
    }
    final p = _bind.agentxx_ffi_drain_logs(_handle, nullptr);
    if (p == nullptr) {
      return null;
    }
    final text = _readCString(p);
    _bind.agentxx_ffi_free(p.cast<Void>());
    try {
      final v = jsonDecode(text);
      return v is List ? v : null;
    } on FormatException {
      return null;
    }
  }

  // -------------------------------------------------------------------------
  // 内部: 通用调用包装
  // -------------------------------------------------------------------------

  /// 运行 [fn] 并捕获 char** log 出参文本; 返回 (fn 结果, log 文本或 null)
  (R, String?) _withLog<R>(R Function(Pointer<Pointer<Char>> logPtr) fn) {
    final logPtr = calloc<Pointer<Char>>();
    try {
      final r = fn(logPtr);
      return (r, _takeLog(logPtr));
    } finally {
      calloc.free(logPtr);
    }
  }

  Map<String, dynamic>? _syncQuery(
    Pointer<Char> Function(
            Pointer<bind.AgentxxFFIAgent>, Pointer<Pointer<Char>>)
        fn,
  ) {
    if (_handle == nullptr) {
      lastQueryError = '未启动';
      return null;
    }
    lastQueryError = null;
    final logPtr = calloc<Pointer<Char>>();
    try {
      final result = fn(_handle, logPtr);
      if (result == nullptr) {
        lastQueryError = _takeLog(logPtr) ?? '查询失败';
        return null;
      }
      final text = _readCString(result);
      _bind.agentxx_ffi_free(result.cast<Void>());
      try {
        final v = jsonDecode(text);
        return v is Map<String, dynamic> ? v : {'raw': text};
      } on FormatException {
        lastQueryError = 'JSON 解析失败: $text';
        return null;
      }
    } finally {
      calloc.free(logPtr);
    }
  }

  // -------------------------------------------------------------------------
  // 内部: 事件轮询循环
  // -------------------------------------------------------------------------

  Future<void> _pollLoop() async {
    // 批量排空模型: 每轮至多处理 maxBatch 条后强制让出一次事件循环。
    // 关键正确性点: OK 分支内没有任何 await —— 若不限制批大小且事件持续
    // 高频到达, while 循环将永不挂起, 主 isolate 被独占, stdin/定时器全部
    // 饥饿。因此积压打满或队列空时都必须 await 交还调度。
    const maxBatchPerYield = 128;

    final typeOut = calloc<Int32>();
    final jsonOut = calloc<Pointer<Char>>();
    try {
      while (_running) {
        final q = _queue;
        if (q == null || q == nullptr) {
          break;
        }

        var drained = 0;
        var queueEmpty = false;
        while (drained < maxBatchPerYield) {
          // timeout=0: 非阻塞探测, 排空当前积压即止
          final rc = _bind.agentxx_ffi_event_queue_pop(q, typeOut, jsonOut, 0);
          switch (rc) {
            case bind.AGENTXX_FFI_OK:
              final payload =
                  jsonOut.value == nullptr ? '' : _readCString(jsonOut.value);
              if (jsonOut.value != nullptr) {
                _bind.agentxx_ffi_free(jsonOut.value.cast<Void>());
              }
              final event = parseAgentEvent(typeOut.value, payload);
              if (event != null && !_eventsController.isClosed) {
                _eventsController.add(event);
              }
              drained++;
            case bind.AGENTXX_FFI_ERR_TIMEOUT:
            case bind.AGENTXX_FFI_ERR_STATE:
              queueEmpty = true;
            default:
              // INVALID 等意外错误: 上报并结束轮询避免死循环
              if (!_eventsController.isClosed) {
                _eventsController.addError(AgentxxException(rc));
              }
              return;
          }
          if (queueEmpty) {
            break;
          }
        }

        if (queueEmpty) {
          // 队列空: 短暂让出事件循环 (输入/定时器得以调度), 兼顾延迟与空转占用
          await Future<void>.delayed(const Duration(milliseconds: 4));
        } else {
          // 积压打满仍有余量: 让出一个事件循环轮次后立即继续排空
          await Future<void>.delayed(Duration.zero);
        }
      }
    } finally {
      calloc.free(typeOut);
      calloc.free(jsonOut);
    }
  }

  void _checkHandle(String what) {
    if (_handle == nullptr) {
      // 以 AgentxxException 而非 StateError 抛出: 交互层统一按可恢复错误
      // 捕获展示 (与 dispose 竞态时不至于因未捕获异常终止进程)
      throw AgentxxException(
          bind.AGENTXX_FFI_ERR_STATE, 'AgentClient 未启动或已销毁, 无法执行: $what');
    }
  }

  static String? _nullIfEmpty(String? s) => (s == null || s.isEmpty) ? null : s;

  // -------------------------------------------------------------------------
  // 内部: 原生内存辅助
  // -------------------------------------------------------------------------

  /// 分配 NUL 结尾 UTF-8 原生缓冲并在 [body] 返回后释放 (参数由原生侧在调用内
  /// 同步拷贝, 故调用返回即缓冲失效); s 为 null 时以 nullptr 调用 [body]
  static R withUtf8<R>(String? s, R Function(Pointer<Char> p) body) {
    if (s == null) {
      return body(nullptr);
    }
    final units = utf8.encode(s);
    final buf = calloc<Uint8>(units.length + 1);
    try {
      if (units.isNotEmpty) {
        buf.asTypedList(units.length).setAll(0, units);
      }
      buf[units.length] = 0; // NUL
      return body(buf.cast<Char>());
    } finally {
      calloc.free(buf);
    }
  }

  /// 读取 NUL 结尾 UTF-8 原生字符串 (Pointer<Char> 无 toDartString 扩展,
  /// package:ffi 的 Utf8 扩展仅作用于 Pointer<Utf8>; 此处手动定长解码)
  String _readCString(Pointer<Char> p) {
    if (p == nullptr) {
      return '';
    }
    var len = 0;
    while (p[len] != 0) {
      len++;
    }
    return utf8.decode(p.cast<Uint8>().asTypedList(len), allowMalformed: true);
  }

  /// 读取并以 agentxx_ffi_free 释放 char** log 出参内容; 无内容时返回 null
  String? _takeLog(Pointer<Pointer<Char>> logPtr) {
    final p = logPtr.value;
    if (p == nullptr) {
      return null;
    }
    final s = _readCString(p);
    _bind.agentxx_ffi_free(p.cast<Void>());
    return s;
  }
}
