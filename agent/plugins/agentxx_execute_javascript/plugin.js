// agentxx_execute_javascript —— JS 代码执行工具插件
// 仿 agentxx_execute_command (agentxx_execute_bash_command / windows) 的设计，
// 提供 agentxx_execute_javascript 工具，在 QuickJS 沙箱内执行任意 JS 代码并返回格式化输出。
// 依赖 agentxx_javascript_engine (interpreter.js 能力) 承载。

(function () {
  var kName = "agentxx_execute_javascript";
  var kMaxStdOut = 30000; // 字符数限制，对齐 execute_command_impl.h kMaxStdOutUtf8Length
  var kMaxStdErr = 30000;
  var _global = (typeof globalThis !== "undefined" ? globalThis : this);

  function countLines(s) {
    if (!s) return 0;
    var n = 1;
    for (var i = 0; i < s.length; i++) {
      if (s.charCodeAt(i) === 10) n++;
    }
    return n;
  }

  function utf8LengthApprox(s) {
    if (typeof TextEncoder !== "undefined") {
      try { return new TextEncoder().encode(s).length; } catch (_) {}
    }
    return s ? s.length : 0;
  }

  function truncateWithStoreFormat(s, maxLen) {
    if (!s) return s;
    var totalLen = utf8LengthApprox(s);
    if (totalLen <= maxLen) return s;

    var targetIndex = maxLen;
    if (targetIndex > s.length) targetIndex = s.length;
    var slice = s.slice(0, targetIndex);
    var lastNL = slice.lastIndexOf("\n");
    var totalLines = countLines(s);
    if (lastNL >= Math.floor(targetIndex / 3) && lastNL >= 0) {
      var lineCount = countLines(s.slice(0, lastNL));
      var header = "[Content offloaded. Use the `agentxx_share_store` tool to fetch the full content. Total " + totalLines + " lines, show [0, " + lineCount + "], hide [" + (lineCount + 1) + ", " + totalLines + "].]";
      return header + "\n" + s.slice(0, lastNL) + "...";
    } else {
      var header = "[Content offloaded. Use the `agentxx_share_store` tool to fetch the full content. Total " + totalLines + " lines.]";
      return header + "\n" + slice + "...";
    }
  }

  function truncateStdOut(s) { return truncateWithStoreFormat(s, kMaxStdOut); }
  function truncateStdErr(s) { return truncateWithStoreFormat(s, kMaxStdErr); }

  function formatSuccess(exitCode, stdoutBuf, stderrBuf, allOutput) {
    var out = "[ExitCode]\n" + exitCode + "\n";
    if (allOutput || exitCode !== 0) {
      var so = stdoutBuf || "";
      var se = stderrBuf || "";
      if (so) so = truncateStdOut(so);
      if (se) se = truncateStdErr(se);
      if (so && so.indexOf("[Content") === 0) {
        out += "[StdOut]" + so + "\n";
      } else {
        out += "[StdOut]\n" + so + "\n";
      }
      if (se && se.indexOf("[Content") === 0) {
        out += "[StdErr]" + se + "\n";
      } else {
        out += "[StdErr]\n" + se + "\n";
      }
    }
    return out;
  }

  function formatTimeout(timeoutSec, stdoutBuf, stderrBuf) {
    var so = stdoutBuf || "";
    var se = stderrBuf || "";
    if (so) so = truncateStdOut(so);
    if (se) se = truncateStdErr(se);
    return "\n[Error]\nCommand timed out after " + timeoutSec + " seconds. If this command is expected to take longer and is not waiting for interactive input, retry with a larger timeout value.\n[StdOut]\n" + so + "\n[StdErr]\n" + se + "\n";
  }

  function stringifyResult(v) {
    if (v === undefined) return "";
    if (v === null) return "null";
    if (typeof v === "string") return v;
    if (typeof v === "number" || typeof v === "boolean") return String(v);
    if (typeof v === "bigint") return String(v);
    try {
      var j = JSON.stringify(v, null, 2);
      if (j !== undefined) return j;
    } catch (_) {}
    try { return String(v); } catch (_) { return "[unstringifiable]"; }
  }

  function toErrorString(e) {
    if (e === null || e === undefined) return String(e);
    if (e instanceof Error) {
      var msg = (e.name ? (e.name + ": ") : "Error: ") + (e.message || String(e));
      if (e.stack) {
        if (e.stack.indexOf(e.message) !== -1) {
          return e.stack;
        }
        return msg + "\n" + e.stack;
      }
      return msg;
    }
    if (typeof e === "string") return e;
    try { return JSON.stringify(e); } catch (_) { return String(e); }
  }

  // --- 核心执行：捕获 console + 执行用户代码 ---
  async function executeJavascript(args, ctx) {
    var rawCode = (args.code !== undefined && args.code !== null) ? args.code
      : (args.command !== undefined && args.command !== null) ? args.command
      : (args.script !== undefined && args.script !== null) ? args.script : "";
    var code = typeof rawCode === "string" ? rawCode : String(rawCode !== undefined && rawCode !== null ? rawCode : "");
    if (!code || !code.trim()) {
      return JSON.stringify({ error: "Arg `code` is empty" });
    }
    var timeout = typeof args.timeout === "number" ? args.timeout : 60;
    var allOutput = typeof args.all_output === "boolean" ? args.all_output : true;

    var stdoutBuf = "";
    var stderrBuf = "";

    var g = _global;
    var origConsole = g.console;
    var captureConsole = {
      log: function() {
        var a = Array.prototype.slice.call(arguments);
        stdoutBuf += a.map(function(x) {
          try { return typeof x === "string" ? x : JSON.stringify(x); } catch (_) { return String(x); }
        }).join(" ") + "\n";
      },
      info: function() {
        var a = Array.prototype.slice.call(arguments);
        stdoutBuf += a.map(function(x) {
          try { return typeof x === "string" ? x : JSON.stringify(x); } catch (_) { return String(x); }
        }).join(" ") + "\n";
      },
      warn: function() {
        var a = Array.prototype.slice.call(arguments);
        stderrBuf += a.map(function(x) {
          try { return typeof x === "string" ? x : JSON.stringify(x); } catch (_) { return String(x); }
        }).join(" ") + "\n";
      },
      error: function() {
        var a = Array.prototype.slice.call(arguments);
        stderrBuf += a.map(function(x) {
          try { return typeof x === "string" ? x : JSON.stringify(x); } catch (_) { return String(x); }
        }).join(" ") + "\n";
      },
      debug: function() {
        var a = Array.prototype.slice.call(arguments);
        stdoutBuf += a.map(function(x) {
          try { return typeof x === "string" ? x : JSON.stringify(x); } catch (_) { return String(x); }
        }).join(" ") + "\n";
      }
    };
    g.console = captureConsole;

    var exitCode = 0;
    var timedOut = false;
    var timeoutId = null;

    try {
      var execPromise = (async function() {
        // 先尝试编译构造函数（不提前执行），避免运行异常误触回退重复执行
        var fn = null;
        try {
          var AsyncFunction = Object.getPrototypeOf(async function () {}).constructor;
          fn = new AsyncFunction('"use strict";\n' + code + '\n');
        } catch (compileErr1) {
          try {
            fn = new Function('"use strict"; return (async function(){\n' + code + '\n})();');
          } catch (compileErr2) {
            // 回退到 eval
            fn = null;
          }
        }

        if (fn) {
          var r = fn();
          if (r && typeof r.then === "function") r = await r;
          return r;
        } else {
          var r2 = eval(code);
          if (r2 && typeof r2.then === "function") r2 = await r2;
          return r2;
        }
      })();

      var finalPromise = execPromise;
      if (timeout > 0) {
        var timeoutPromise = new Promise(function(_, reject) {
          timeoutId = setTimeout(function() {
            timedOut = true;
            reject(new Error("timeout:" + timeout));
          }, timeout * 1000);
        });
        finalPromise = Promise.race([execPromise, timeoutPromise]);
      }

      var value;
      try {
        value = await finalPromise;
      } finally {
        if (timeoutId !== null) clearTimeout(timeoutId);
      }

      if (!timedOut) {
        var s = stringifyResult(value);
        if (s) {
          if (stdoutBuf && stdoutBuf.lastIndexOf("\n") !== stdoutBuf.length - 1) {
            stdoutBuf += "\n";
          }
          stdoutBuf += s;
          if (stdoutBuf.lastIndexOf("\n") !== stdoutBuf.length - 1) {
            stdoutBuf += "\n";
          }
        }
      }
    } catch (e) {
      var isTimeout = e && e.message && String(e.message).indexOf("timeout:") === 0;
      if (isTimeout) {
        timedOut = true;
      } else {
        exitCode = 1;
        stderrBuf += toErrorString(e) + "\n";
      }
    } finally {
      if (origConsole) g.console = origConsole;
      else delete g.console;
      if (timeoutId !== null) {
        try { clearTimeout(timeoutId); } catch (_) {}
      }
    }

    if (timedOut) {
      return formatTimeout(timeout, stdoutBuf, stderrBuf);
    }
    return formatSuccess(exitCode, stdoutBuf, stderrBuf, allOutput);
  }

  var toolSpec = {
    name: kName,
    description: "Execute JavaScript code and return its output. JS equivalent of agentxx_execute_bash_command — runs code in the QuickJS sandbox and captures console output / return value / errors with timeout and truncation.",
    parameters: {
      type: "object",
      properties: {
        code: {
          type: "string",
          description: "The JavaScript code to execute. The code is run as an async function body (strict mode), so you can use `await`, `return`, top-level statements, or expressions. Example: `return 2+3`, `console.log('hi'); return {a:1}`, `await new Promise(r=>setTimeout(r,10)); return 42`. Aliases `command`/`script` are also accepted for compatibility."
        },
        timeout: {
          type: "integer",
          default: 60,
          description: "Default `60` seconds. Execution timeout in seconds. Set `0` for no limit. On timeout returns [Error] with partial StdOut/StdErr."
        },
        all_output: {
          type: "boolean",
          default: true,
          description: "Default `true`. `true`: Always return StdOut and StdErr. `false`: Only return output when execution fails (non-zero exit)."
        }
      },
      required: ["code"]
    },
    execute: executeJavascript
  };

  agentxx.registerTool(toolSpec);

  try {
    agentxx.registerTool({
      name: "agentxx_execute_js",
      description: "Alias of agentxx_execute_javascript — execute JavaScript code in QuickJS sandbox.",
      parameters: toolSpec.parameters,
      execute: executeJavascript
    });
  } catch (_) {}

  agentxx.log(2, kName + " plugin loaded (JS execution tool ready, mimics execute_bash_command).");
})();
