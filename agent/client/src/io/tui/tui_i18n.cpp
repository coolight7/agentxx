#include "agentxx-client/io/tui/framework/tui_i18n.h"

#include "agentxx-client/io/tui/framework/tui_settings.h"
#include <unordered_map>

// ---------------------------------------------------------------------------
// 翻译表
//
// 条目 = { key, en, zh }: en 列等于原英文界面文本 (默认回退), zh 列是简体中文。
// 界面代码只引用 key, 展示文本完全由当前语言决定 (见 TuiI18n::t)。
//
// 约定 (与 tui_i18n.h 一致):
// - 消息角色标记 ([Think]/[Tool]/[System]/[Permission]/[Interrupt] 等) 与
//   日志前缀/协议字段标签 (args:/result:/tool_calls: 等) 不翻译, 两列相同
// - 插件提供的内容 (工具名/面板/Info 段) 由插件方决定, 不经本表
// - 含格式占位符的条目用 {} (fmt 语义), 调用侧经 TuiI18n::t(key, args...)
//   填充
// ---------------------------------------------------------------------------

namespace {

struct Entry {
    const char* key;
    const char* en;
    const char* zh;
};

constexpr Entry kTable[] = {
    // ---- banner (消息列表空状态) ----
    {"banner.connecting",   "server-io is starting...",                     "server-io 正在启动中..."        },
    {"banner.failed",       "  server-io connection failed  ",              "  server-io 连接失败  "         },
    {"banner.retry",        "  [ Retry ]  ",                                "  [ 重试 ]  "                   },
    {"banner.connected",
     "Type a message to start. [Esc] cancel, [Ctrl+C] quit.",
     "输入消息以开始对话。[Esc] 取消, [Ctrl+C] 退出。"                                                     },

    // ---- toast ----
    {"toast.notReady",      "server-io is not ready yet, please try later", "server-io 尚未就绪, 请稍后再试" },
    {"toast.stopCurrent",   "Please stop the current session first",        "请先停止当前会话"               },
    {"toast.stopToSwitch",
     "Please stop the current session before switching",
     "请先停止当前会话, 再进行会话切换"                                                                    },
    {"toast.copied",        "Copied ({})",                                  "已复制 ({})"                    },
    {"toast.copyFailed",    "Copy failed (clipboard unavailable)",          "复制失败 (剪贴板不可用)"         },

    // ---- 待发送消息队列 (顶栏 + 弹窗) ----
    {"queue.barTitle",      "  · Message Queue: {}",                        "  · 待发送消息队列: {}"         },
    {"queue.insert",        " [insert] ",                                   " [立即发送] "                   },
    {"queue.title",         " Pending Message Queue ",                      " 待发送消息队列 "                },
    {"queue.clear",         " Clear ",                                      " 清空 "                         },
    {"queue.empty",         " (empty) ",                                    " (空) "                         },
    {"queue.hint",
     " Click message to expand/collapse  Click ✕ to delete  [Esc] Close ",
     " 点击消息展开/折叠  点击 ✕ 删除  [Esc] 关闭 "                                                     },

    // ---- 输入框 ----
    {"input.placeholder",
     "Type a message... (ESC:Interrupt, Enter:Send, Alt+Enter:Newline)",
     "输入消息... (Esc: 中断, Enter: 发送, Alt+Enter: 换行)"                                               },

    // ---- 模型选择弹窗 ----
    {"model.title",         " Select Model ",                               " 选择模型 "                     },
    {"model.loading",       "Loading models...",                            "模型加载中..."                  },
    {"model.empty",         "(no models available)",                        "(无可用模型)"                   },
    {"model.hint",
     " [Up/Down] Move  [Enter] Select  [Esc] Cancel ",
     " [方向键] 移动  [Enter] 选择  [Esc] 取消 "                                                         },

    // ---- 会话选择弹窗 ----
    {"session.title",       " Select Session ",                             " 选择会话 "                     },
    {"session.new",         "+ New Session",                                "+ 新会话"                       },
    {"session.loading",     "Loading sessions...",                          "会话加载中..."                  },
    {"session.empty",       "(no persisted sessions)",                      "(无已保存会话)"                 },
    {"session.current",     "{} (current)",                                 "{} (当前)"                      },
    {"session.loadingMore", "↓ Loading...",                                 "↓ 加载中..."                    },
    {"session.loadedMore",
     "Loaded {}/{}  ↓ Scroll down for more",
     "已加载 {}/{}  ↓ 下移加载更多"                                                                       },
    {"session.loadMore",    "↓ Scroll down for more",                       "↓ 下移加载更多"                 },
    {"session.hint",
     " [Up/Down] Move  [Enter] Switch  [Esc] Cancel ",
     " [方向键] 移动  [Enter] 切换  [Esc] 取消 "                                                         },

    // ---- 设置弹窗 ----
    {"settings.title",      " Settings ",                                    " 设置 "                         },
    {"settings.themeLabel", " Theme ",                                       " 主题 "                         },
    {"settings.themeValue", " Theme: {} ",                                   " 主题: {} "                     },
    {"settings.animLabel",  " Animation ",                                   " 动画 "                         },
    {"settings.animValue",  " Animation Level: {} ",                         " 动画等级: {} "                 },
    {"settings.logLabel",   " Log ",                                         " 日志 "                         },
    {"settings.logValue",   " Log Level: {} ",                               " 日志等级: {} "                 },
    {"settings.thinkLabel", " Thinking ",                                    " 思考 "                         },
    {"settings.thinkValue", " Tail Thinking: {} ",                           " 末尾思考: {} "                 },
    {"settings.langLabel",  " Language ",                                    " 语言 "                         },
    {"settings.langValue",  " Language: {} ",                                " 语言: {} "                     },
    {"settings.infoLabel",  " Info ",                                        " 信息 "                         },
    {"settings.aboutValue", " About ",                                       " 关于 "                         },
    {"settings.hint",
     " [Up/Down] Move  [Enter] Toggle  [Esc] Close ",
     " [方向键] 移动  [Enter] 切换  [Esc] 关闭 "                                                         },

    // ---- Logs 侧边栏 Menu 弹窗 ----
    {"menu.title",          " Menu ",                                        " 菜单 "                         },
    {"menu.llmContext",     "LLM Context",                                   "LLM 上下文"                    },
    {"menu.summaryContext", "Summy Context",                                 "总结上下文"                    },
    {"menu.clearLogs",      "Clear Logs",                                    "清空日志"                      },
    {"menu.hint",
     " [Up/Down] Select  [Enter] Confirm  [Esc] Close ",
     " [方向键] 选择  [Enter] 确认  [Esc] 关闭 "                                                         },

    // ---- About 弹窗 ----
    {"about.title",         " About ",                                       " 关于 "                         },
    {"about.develop",       "Develop",                                       "开发"                           },
    {"about.execPath",      "Executable Path",                               "可执行文件路径"                 },
    {"about.serverIoType",  "Server-IO Type",                                "Server-IO 类型"                 },
    {"about.dataDir",       "Data Directory (data_dir)",                     "数据目录 (data_dir)"            },
    {"about.workDir",       "Session Working Directory",                     "会话工作目录"                   },
    {"about.builtinPlugins","Builtin Plugins ({})",                          "内嵌插件 ({})"                  },
    {"about.loadedPlugins", "Loaded Plugins ({})",                           "已加载插件 ({})"                },
    {"about.innerServer",   "Inner Server (In-process)",                     "内置服务 (进程内)"              },
    {"about.remote",        "Remote ({})",                                   "远程 ({})"                      },
    {"about.remoteNoCfg",   "(remote / not configured)",                     "(远程 / 未配置)"                },
    {"about.none",          "(none)",                                        "(无)"                           },
    {"about.hint",
     " [Wheel/Up/Down] Scroll  [Esc/Enter] Close ",
     " [滚轮/方向键] 滚动  [Esc/Enter] 关闭 "                                                             },

    // ---- 上下文弹窗 ----
    {"ctx.title",           " LLM Context · {}",                             " LLM 上下文 · {}"               },
    {"ctx.empty",           " (empty) ",                                     " (空) "                         },
    {"ctx.hint",
     " [Click/Enter/Space] Toggle  [Wheel/Up/Down] Scroll  [PgUp/PgDn] Page  [Esc] Close ",
     " [点击/Enter/空格] 展开或折叠  [滚轮/方向键] 滚动  [PgUp/PgDn] 翻页  [Esc] 关闭 "                     },

    // ---- Mermaid 状态图弹窗 ----
    {"graph.title",         " Graph ",                                       " 状态图 "                       },
    {"graph.noDiagram",     " (no diagram) ",                                " (无状态图) "                   },

    // ---- 通用滚动弹窗提示 (Mermaid/加载失败共用) ----
    {"overlay.scrollHint",
     " [Wheel/Up/Down] Scroll  [Esc] Close ",
     " [滚轮/方向键] 滚动  [Esc] 关闭 "                                                                   },

    // ---- 加载失败组件弹窗 ----
    {"failed.title",        " Failed Components ",                           " 加载失败的组件 "               },
    {"failed.empty",        " (no failed components) ",                      " (无失败组件) "                 },
    {"failed.unknownType",  "Unknown",                                       "未知"                           },

    // ---- 中断输入控件 ----
    {"interrupt.header",    "! [Interrupt] Input {}/{}: ",                   "! [中断] 输入 {}/{}: "          },
    {"interrupt.confirmed", "Confirmed {}: {}",                              "已确认 {}: {}"                  },
    {"interrupt.cancelled", "{}: Cancelled",                                 "{}: 已取消"                     },
    {"interrupt.expired",   "{}: Expired",                                   "{}: 已过期"                     },
    {"interrupt.yes",       " Yes ",                                         " 是 "                           },
    {"interrupt.no",        " No ",                                          " 否 "                           },
    {"interrupt.confirm",   " [Confirm] ",                                   " [确认] "                       },
    {"interrupt.cancel",    " ✕ ",                                           " ✕ "                            },
    {"interrupt.allow",     "[ Allow ]",                                     "[ 允许 ]"                       },
    {"interrupt.deny",      "[ Deny ]",                                      "[ 拒绝 ]"                       },
    {"interrupt.remember",  "Remember this choice",                          "记住此选择"                     },
    {"interrupt.tipInt",    "Invalid integer, please input again.",          "无效整数, 请重新输入。"          },
    {"interrupt.tipNum",    "Invalid number, please input again.",           "无效数字, 请重新输入。"          },

    // ---- 思考消息 (加密思考占位) ----
    {"think.encryptedTokens","encrypted thinking {} tokens",                 "加密思考 {} tokens"             },
    {"think.encrypted",     "Thinking content is encrypted",                 "思考内容被加密"                 },

    // ---- 工具消息正文标签 (字段前缀, 保持两语言一致) ----
    {"tool.args",           "  args: ",                                      "  args: "                       },
    {"tool.result",         "  result: ",                                    "  result: "                     },
    {"tool.file",           "  file: ",                                      "  file: "                       },
    {"tool.running",        "  running...",                                  "  running..."                   },
    {"tool.noChanges",      "  (no changes)",                                "  (no changes)"                 },

    // ---- 状态栏 ----
    {"status.modelNone",    "<none>",                                        "<none>"                        },
    {"status.sessions",     "[F4] Sessions",                                 "[F4] 会话"                      },
    {"status.settings",     "[F3] Settings",                                 "[F3] 设置"                      },

    // ---- 侧边栏 (Info/Logs 常驻标签) ----
    {"sidebar.info",        "Info",                                          "信息"                           },
    {"sidebar.logs",        "Logs",                                          "日志"                           },

    // ---- 日志/Info 侧边栏内容 ----
    {"info.empty",          "[Empty]",                                       "[空]"                           },
    {"info.append",         "Append",                                        "加载组件"                       },
    {"info.appendFailed",   "Failed: {}",                                    "失败: {}"                       },
    {"info.viewFailed",     " [view] ",                                      " [查看] "                       },
    {"info.graphButton",    " Graph ",                                       " 图 "                           },
    {"info.workDirUnknown", "[Unknown Work Dir]",                            "[未知工作目录]"                 },
    {"info.idle",           "idle",                                          "空闲"                           },
    {"footer.menu",         " Menu ",                                        " 菜单 "                         },
};

/// key → (en, zh) 静态查找表 (仅构建一次; 值指向静态字符串字面量)
struct LangTables {
    std::unordered_map<std::string_view, std::pair<std::string_view, std::string_view>> map;

    LangTables() {
        map.reserve(std::size(kTable));
        for (const auto& e : kTable) {
            map.emplace(e.key, std::make_pair(std::string_view{e.en}, std::string_view{e.zh}));
        }
    }
};

const LangTables& tables() {
    static const LangTables t;
    return t;
}

} // namespace

TuiI18n& TuiI18n::instance() {
    static TuiI18n inst;
    return inst;
}

std::string_view TuiI18n::t(std::string_view key) const noexcept {
    const auto& tbl = tables().map;
    const auto  it  = tbl.find(key);
    if (it == tbl.end()) {
        return key; // 未配置的 key 原样返回, 便于尽早发现漏配
    }
    const TuiLanguage lang = TUISettings::instance().effectiveLanguage();
    return (lang == TuiLanguage::EnUs) ? it->second.first : it->second.second;
}
