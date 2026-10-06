#include "agentxx-client/io/tui/framework/tui_i18n.h"

#include "agentxx-client/io/tui/framework/tui_settings.h"
#include <unordered_map>

namespace agentxx::client {

// ---------------------------------------------------------------------------
// 翻译表
//
// 条目 = { key, en, zh }: en 列等于原英文界面文本 (默认回退), zh 列是简体中文。
// 界面代码只引用 key, 展示文本完全由当前语言决定 (见 TuiI18n::t)。
//
// 约定 (与 tui_i18n.h 一致):
// - 日志前缀与协议字段标签 (role/args:/result:/
//   tool_calls: 等) 属技术字段, 不翻译
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
    {"banner.connecting",               "server-io is starting...",                                                      "server-io 正在启动中..."                                                                             },
    {"banner.failed",                   "  server-io connection failed  ",                                               "  server-io 连接失败  "                                                                               },
    {"banner.retry",                    "[ Retry ]",                                                                     "[ 重试 ]"                                                                                               },
    {"banner.connected",
     "Type a message to start. [Esc] interrupt, [Ctrl+C] quit.",                                                         "输入消息以开始对话。[Esc] 中断, [Ctrl+C] 退出。"                                           },

 // ---- toast ----
    {"toast.notReady",
     "server-io is not ready yet, please try later",                                                                     "server-io 尚未就绪, 请稍后再试"                                                                  },
    {"toast.stopCurrent",               "Please stop the current session first",                                         "请先停止当前会话"                                                                                 },
    {"toast.stopToSwitch",
     "Please stop the current session before switching",                                                                 "请先停止当前会话, 再进行会话切换"                                                          },
    {"toast.copied",                    "Copied ({})",                                                                   "已复制 ({})"                                                                                           },
    {"toast.copyFailed",                "Copy failed (clipboard unavailable)",                                           "复制失败 (剪贴板不可用)"                                                                        },
    {"toast.attachNotSupported",
     "Current model does not support multimodal file input",                                                             "当前模型不支持多模态文件输入"                                                               },
    {"toast.attachLimit",               "Max attachments per message (5) reached",                                       "已达到单次对话最大附件数 (5)"
    },
    {"toast.attachReadFail",            "Cannot read file: {}",                                                          "无法读取文件: {}"                                                                                   },
    {"toast.attachTooLarge",            "File too large ({}, limit {})",                                                 "文件过大 ({}, 限制 {})"                                                                             },
    {"toast.attachOpenFail",            "Cannot open file",                                                              "无法打开文件"                                                                                       },
    {"toast.attachBadType",             "Unsupported file type",                                                         "不支持的文件类型"                                                                                 },
    {"toast.fullAuthOn",
     "Full authorization enabled: permission requests will no longer be asked",                                          "已切换为完全授权: 后续不再询问权限"                                                       },
    {"toast.fullAuthOff",
     "Authorization prompt restored: permission requests will be asked again",                                           "已恢复询问授权: 权限请求将再次询问"                                                       },
    {"toast.updateAvailable",
     "New version {} available (current {}); click the notice in the Info sidebar to copy the link",                     "发现新版本 {} (当前 {}); 点击 Info 侧边栏提示可复制链接"                               },
    {"toast.updateChecking",            "Checking for updates...",                                                       "正在检查更新..."                                                                                    },
    {"toast.updateLatest",              "Already on the latest version",                                                 "已经是最新版本"                                                                                    },
    {"toast.updateCheckFailed",         "Update check failed: {}",                                                       "检查更新失败: {}"                                                                                   },
    {"toast.updateOpenFail",
     "Cannot open the browser (select the link to copy it)",                                                             "无法打开浏览器 (可拖选链接复制)"                                                            },
    {"toast.modelAdded",                "Model {} added and switched to it",                                             "已添加模型 {} 并切换使用"                                                                       },
    {"toast.modelAddFailed",            "Add model failed: {}",                                                          "添加模型失败: {}"                                                                                   },
    {"toast.inputRejected",             "Input not accepted: {}",                                                        "输入未被受理: {}"                                                                                   },

 // ---- 待发送消息队列 (顶栏 + 弹窗) ----
    {"queue.barTitle",                  "  • Message Queue: {}",                                                       "  • 待发送消息队列: {}"                                                                          },
    {"queue.insert",                    "[ Insert ]",                                                                    "[ 立即发送 ]"                                                                                         },
    {"queue.title",                     "Pending Message Queue",                                                         "待发送消息队列"                                                                                    },
    {"queue.clear",                     "[ Clear ]",                                                                     "[ 清空 ]"                                                                                               },
    {"queue.empty",                     "( empty )",                                                                     "( 空 )"                                                                                                  },
    {"queue.attachCount",               "[ @ x{} ]",                                                                     "[ @ x{} ]"                                                                                                },
    {"queue.hint",
     " Click message to expand/collapse  Click ✕ to delete  [Esc] Close ",                                             " 点击消息展开/折叠  点击 ✕ 删除  [Esc] 关闭 "                                             },

 // ---- 输入框 ----
    {"input.placeholder",
     "Type a message... [ESC] Interrupt [Enter] Send [Alt+Enter] Newline",                                               "输入消息... [Esc] 中断 [Enter] 发送 [Alt+Enter] 换行"                                           },
    {"input.attach",                    "[ @ ]",                                                                         "[ @ ]"                                                                                                    },
    {"input.attachTray",                " @ Pending ({}): ",                                                             " @ 待发附件 ({}): "                                                                                   },
    {"msg.attachImage",                 "📷︎ Image",                                                                 "📷︎ 图像附件"                                                                                     },
    {"msg.attachAudio",                 "🎵︎ Audio",                                                                 "🎵︎ 音频附件"                                                                                     },
    {"msg.attachVideo",                 "🎬︎ Video",                                                                 "🎬︎ 视频附件"                                                                                     },
    {"msg.attachOpen",                  "( Click to show )",                                                             "( 点击显示 )"                                                                                         },

 // ---- 文件选择弹窗 ----
    {"picker.title",                    "Select File ( model supports: {} )",                                            "选择文件 ( 当前模型支持: {} )"                                                                  },
    {"picker.path",                     "Path: ",                                                                        "路径: "                                                                                                 },
    {"picker.image",                    " Image 📷︎ ",                                                               " 图像 📷︎ "                                                                                         },
    {"picker.audio",                    " Audio 🎵︎ ",                                                               " 音频 🎵︎ "                                                                                         },
    {"picker.video",                    " Video 🎬︎ ",                                                               " 视频 🎬︎ "                                                                                         },
    {"picker.parent",                   "[..] Parent",                                                                   "[..] 上级目录"                                                                                        },
    {"picker.empty",                    "( no matching media files )",                                                   "( 无匹配的媒体文件 )"                                                                             },
    {"picker.unsupported",              "( model does not support {} ) ",                                                "( 当前模型不支持{} ) "                                                                             },
    {"picker.tab_local",                " 💻 Local ",                                                                  " 💻 本地 "                                                                                            },
    {"picker.tab_server",               " 🌐 Server ",                                                                 " 🌐 服务端 "                                                                                         },
    {"picker.server_loading",           "( Loading server files... )",                                                   "( 正在加载服务端文件... )"                                                                       },
    {"picker.server_error",             "Failed to load: {}",                                                            "加载失败: {}"                                                                                         },
    {"picker.hint",
     " [Up/Down] Move [Enter] Select/Enter dir [Esc] Cancel ",                                                           " [↑/↓] 移动光标 [Enter] 确认选择/进入目录 [Esc] 取消/关闭 "                           },
    {"picker.hint_tabs",
     " [Tab] Switch device [↑/↓] Move [Enter] Select/Enter dir [Esc] Cancel ",                                       " [Tab] 切换设备 [↑/↓] 移动光标 [Enter] 确认选择/进入目录 [Esc] 取消/关闭 "        },

 // ---- 模型选择弹窗 ----
    {"model.title",                     "Select Model",                                                                  "选择模型"                                                                                             },
    {"model.loading",                   "( Loading models... )",                                                         "( 模型加载中... )"                                                                                   },
    {"model.empty",                     "( no models available )",                                                       "( 无可用模型 )"                                                                                      },
    {"model.hint",
     " [Up/Down] Move [Enter] Select [Esc] Cancel ",                                                                     " [方向键] 移动 [Enter] 选择 [Esc] 取消 "                                                         },
    {"model.add",                       "[ + Add Model Config ]",                                                        "[ + 添加模型配置 ]"                                                                                 },

 // ---- 添加模型配置弹窗 ----
    {"model.form.title",                "Add Model Config",                                                              "添加模型配置"                                                                                       },
    {"model.form.hint",
     " [Tab] Next field [Enter] Save [Esc] Cancel ",                                                                     " [Tab] 切换字段 [Enter] 保存 [Esc] 取消 "                                                         },
    {"model.form.desc",
     "Saved to agentxx-config.yaml in the server data directory (created when missing), switched to right after saving",
     "保存到服务端数据目录的 agentxx-config.yaml (文件不存在时创建), 保存成功后立即切换使用"                                                                                                          },
    {"model.form.advanced",             "— Advanced —",                                                              "— 高级选项 —"                                                                                     },
    {"model.form.name",                 "Name *",                                                                        "名称 *"                                                                                                 },
    {"model.form.nameHelp",
     "Model id shown in the model list; must be unique",                                                                 "模型标识, 弹窗列表显示名; 需唯一 (建议英文/数字/下划线)"                           },
    {"model.form.type",                 "Type",                                                                          "类型"                                                                                                   },
    {"model.form.typeHelp",
     "API type; decides the API path and request/response format",                                                       "接口类型; 决定 API 路径与请求/响应格式"                                                    },
    {"model.form.baseUrl",              "API Base URL",                                                                  "API 地址"                                                                                               },
    {"model.form.baseUrlHelp",
     "e.g. https://api.example.com/v1; empty uses the official address of the type",                                     "如 https://api.example.com/v1; 留空则用该类型的官方地址"                                     },
    {"model.form.apiPath",              "API Path",                                                                      "API 路径"                                                                                               },
    {"model.form.apiPathHelp",
     "Optional; auto by type when empty (openai: /chat/completions, responses: /responses)",                             "可选; 留空按类型自动选择 (openai: /chat/completions, responses: /responses)"                   },
    {"model.form.apiKey",               "API Key",                                                                       "API Key"                                                                                                  },
    {"model.form.apiKeyHelp",
     "Use EMPTY for services without auth; ${ENV_NAME} is also accepted",                                                "无鉴权服务填 EMPTY; 也可写成 ${ENV_NAME} 引用环境变量"                                    },
    {"model.form.modelName",            "Model Name",                                                                    "模型名"                                                                                                },
    {"model.form.modelNameHelp",
     "Value of the `model` field in the request body, e.g. deepseek-chat",                                               "请求体里 model 字段的值, 如 deepseek-chat"                                                       },
    {"model.form.contextToken",         "Context Token Limit",                                                           "上下文 token 上限"                                                                                   },
    {"model.form.contextTokenHelp",
     "0 = not specified (context compression uses its default)",                                                         "0 = 未指定 (上下文压缩用默认值)"                                                              },
    {"model.form.connectTimeout",       "Connect Timeout (s)",                                                           "连接超时 (秒)"                                                                                       },
    {"model.form.connectTimeoutHelp",
     "Timeout for establishing the HTTP connection",                                                                     "建立 HTTP 连接的超时时间"                                                                        },
    {"model.form.readTimeout",          "Read Timeout (s)",                                                              "读取超时 (秒)"                                                                                       },
    {"model.form.readTimeoutHelp",
     "Longest gap between two response data chunks",                                                                     "相邻响应数据分段之间的最长间隔"                                                            },
    {"model.form.maxConnections",       "Max Concurrent Connections",                                                    "最大并发连接数"                                                                                    },
    {"model.form.maxConnectionsHelp",
     "Connection pool limit of this endpoint; 0 = unlimited",                                                            "该 API 端点的连接池上限; 0 = 不限制"                                                          },
    {"model.form.sendThinking",         "Send Thinking",                                                                 "发送 thinking"                                                                                          },
    {"model.form.sendThinkingHelp",
     "Carry thinking content in requests (some models require it)",                                                      "请求时携带思考内容 (部分模型要求开启才能正常对话)"                                 },
    {"model.form.reasoningSummary",     "Request Reasoning Summary",                                                     "请求思考摘要"                                                                                       },
    {"model.form.reasoningSummaryHelp",
     "Responses API `include`; disable when the upstream rejects reasoning.summary_text (HTTP 400)",                     "Responses API 的 include 参数; 上游不支持 reasoning.summary_text 时需关闭, 否则 API 报 400"},
    {"model.form.sslVerify",            "TLS Verify",                                                                    "TLS 证书校验"                                                                                         },
    {"model.form.sslVerifyHelp",        "Default = follow the global setting",                                           "默认 = 跟随全局设置"                                                                              },
    {"model.form.sslDefault",           "Default",                                                                       "默认"                                                                                                   },
    {"model.form.sslOn",                "On",                                                                            "开启"                                                                                                   },
    {"model.form.sslOff",               "Off",                                                                           "关闭"                                                                                                   },
    {"model.form.imageInput",           "Image Input",                                                                   "图片输入"                                                                                             },
    {"model.form.imageInputHelp",
     "Model accepts images (turns on the attach button in the input bar)",                                               "该模型支持图片输入 (开启后输入栏出现附件按钮)"                                       },
    {"model.form.audioInput",           "Audio Input",                                                                   "音频输入"                                                                                             },
    {"model.form.audioInputHelp",       "Model accepts audio",                                                           "该模型支持音频输入"                                                                              },
    {"model.form.videoInput",           "Video Input",                                                                   "视频输入"                                                                                             },
    {"model.form.videoInputHelp",       "Model accepts video",                                                           "该模型支持视频输入"                                                                              },
    {"model.form.extraHeaders",         "Extra Headers (JSON)",                                                          "额外请求头 (JSON)"                                                                                   },
    {"model.form.extraHeadersHelp",
     "Optional; e.g. {\"X-Gateway\":\"xxx\"}; empty means none",                                                         "可选; 形如 {\"X-Gateway\":\"xxx\"}, 留空表示不添加"                                            },
    {"model.form.extraConfig",          "Extra API Config (JSON)",                                                       "额外 API 参数 (JSON)"                                                                                 },
    {"model.form.extraConfigHelp",
     "Optional; merged into the request body, e.g. {\"reasoning_effort\":\"high\"}",                                     "可选; 合并进请求体, 如 {\"reasoning_effort\":\"high\"}"                                          },
    {"model.form.errName",              "Model name is required",                                                        "模型名称不能为空"                                                                                 },
    {"model.form.errJson",              "Invalid JSON",                                                                  "JSON 格式非法"                                                                                        },
    {"model.form.errJsonObject",        "Must be a JSON object",                                                         "必须是 JSON 对象"                                                                                    },
    {"model.form.errHeaderValue",       "Header values must be strings",                                                 "请求头的值必须是字符串"                                                                        },
    {"model.form.errSend",
     "Cannot send request: server connection is not ready",                                                              "无法发送请求: 服务端连接尚未就绪"                                                          },

 // ---- 会话选择弹窗 ----
    {"session.title",                   "Select Session",                                                                "选择会话"                                                                                             },
    {"session.new",                     "[ + New Session ]",                                                             "[ + 新会话 ]"                                                                                          },
    {"session.loading",                 "( Loading sessions... )",                                                       "( 会话加载中... )"                                                                                   },
    {"session.empty",                   "( no persisted sessions )",                                                     "( 无已保存会话 )"                                                                                   },
    {"session.current",                 "{} ( current )",                                                                "{} ( 当前 )"                                                                                            },
    {"session.loadingMore",             "↓ Loading...",                                                                "↓ 加载中..."                                                                                         },
    {"session.loadedMore",              "Loaded {}/{}  ↓ Scroll down for more",                                        "已加载 {}/{}  ↓ 下移加载更多"                                                                  },
    {"session.loadMore",                "↓ Scroll down for more",                                                      "↓ 下移加载更多"                                                                                   },
    {"session.hint",
     " [Up/Down] Move [Enter] Switch [Esc] Cancel ",                                                                     " [方向键] 移动 [Enter] 切换 [Esc] 取消 "                                                         },

 // ---- 设置弹窗 ----
    {"settings.title",                  "Settings",                                                                      "设置"                                                                                                   },
    {"settings.groupInterface",         "Interface",                                                                     "界面"                                                                                                   },
    {"settings.groupDisplay",           "Display",                                                                       "显示"                                                                                                   },
    {"settings.groupUpdate",            "Update",                                                                        "更新"                                                                                                   },
    {"settings.groupOther",             "Other",                                                                         "其他"                                                                                                   },
    {"settings.themeValue",             "Theme: {}",                                                                     "主题: {}"                                                                                               },
    {"settings.animValue",              "Animation Level: {}",                                                           "动画等级: {}"                                                                                         },
    {"settings.logValue",               "Log Level: {}",                                                                 "日志等级: {}"                                                                                         },
    {"settings.thinkValue",             "Tail Thinking: {}",                                                             "末尾思考显示: {}"                                                                                   },
    {"settings.langValue",              "Language: {}",                                                                  "语言: {}"                                                                                               },
    {"settings.aboutValue",             "About",                                                                         "关于"                                                                                                   },
    {"settings.keybindValue",           "Plugin Keybinds: {}",                                                           "插件快捷键: {}"                                                                                      },
    {"settings.updateValue",            "Check for Updates on Startup: {}",                                              "启动时检查更新: {}"                                                                                },
    {"settings.checkUpdateValue",       "Check Now",                                                                     "立即检查"                                                                                             },
    {"settings.switchOn",               "On",                                                                            "开"                                                                                                      },
    {"settings.switchOff",              "Off",                                                                           "关"                                                                                                      },
    {"settings.hint",
     " [Up/Down] Move [Enter] Toggle [Esc] Close ",                                                                      " [方向键] 移动 [Enter] 切换 [Esc] 关闭 "                                                         },

 // ---- 更新提示弹窗 (设置弹窗"检查更新"发现新版本时) ----
    {"update.title",                    "Update Available",                                                              "发现新版本"                                                                                          },
    {"update.versionLine",              "New version {} -> {}",                                                          "新版本 {} -> {}"                                                                                       },
    {"update.download",                 "[ Download ]",                                                                  "[ 前往下载 ]"                                                                                         },
    {"update.hint",                     " [Enter] Download [Esc] Close ",                                                " [Enter] 前往下载 [Esc] 关闭 "                                                                      },

 // ---- 插件快捷键列表弹窗 (只读) ----
    {"keybind.title",                   "Plugin Keybinds",                                                               "插件快捷键"                                                                                          },
    {"keybind.hint",                    " [Up/Down] Scroll [Esc] Close ",                                                " [方向键] 滚动 [Esc] 关闭 "                                                                        },
    {"keybind.empty",                   "No global shortcuts registered by plugins",                                     "暂无插件注册的全局快捷键"                                                                     },
    {"keybind.noDesc",                  "( no description )",                                                            "( 无说明 )"                                                                                            },
    {"keybind.conflictTitle",           "Key Conflicts ({})",                                                            "键位冲突 ({})"                                                                                        },
    {"keybind.conflictLine",            "requested by {} · held by {}",                                                 "申请方 {} · 占用方 {}"                                                                             },

 // ---- Logs 侧边栏 Menu 弹窗 ----
    {"menu.title",                      "Menu",                                                                          "菜单"                                                                                                   },
    {"menu.llmContext",                 "LLM Context",                                                                   "LLM 上下文"                                                                                            },
    {"menu.summaryContext",             "Summary Context",                                                               "总结上下文"                                                                                          },
    {"menu.clearLogs",                  "Clear Logs",                                                                    "清空日志"                                                                                             },
    {"menu.hint",
     " [Up/Down] Select [Enter] Confirm [Esc] Close ",                                                                   " [方向键] 选择 [Enter] 确认 [Esc] 关闭 "                                                         },

 // ---- About 弹窗 ----
    {"about.title",                     "About",                                                                         "关于"                                                                                                   },
    {"about.version",                   "Version",                                                                       "版本"                                                                                                   },
    {"about.develop",                   "Develop",                                                                       "开发"                                                                                                   },
    {"about.execPath",                  "Executable Path",                                                               "可执行文件路径"                                                                                    },
    {"about.serverIoType",              "Server-IO Type",                                                                "Server-IO 类型"                                                                                         },
    {"about.dataDir",                   "Data Directory (data_dir)",                                                     "数据目录 (data_dir)"                                                                                  },
    {"about.workDir",                   "Session Working Directory",                                                     "会话工作目录"                                                                                       },
    {"about.builtinPlugins",            "Builtin Plugins ({})",                                                          "内嵌插件 ({})"                                                                                        },
    {"about.loadedPlugins",             "Loaded Plugins ({})",                                                           "已加载插件 ({})"                                                                                     },
    {"about.innerServer",               "Inner Server",                                                                  "内置服务"                                                                                             },
    {"about.remote",                    "Remote {}",                                                                     "远程 {}"                                                                                                },
    {"about.remoteNoCfg",               "( remote / not configured )",                                                   "( 远程 / 未配置 )"                                                                                   },
    {"about.none",                      "( none )",                                                                      "( 无 )"                                                                                                  },
    {"about.hint",
     " [Wheel/Up/Down] Scroll [Esc/Enter] Close ",                                                                       " [滚轮/方向键] 滚动 [Esc/Enter] 关闭 "                                                           },

 // ---- 上下文弹窗 ----
    {"ctx.title",                       "LLM Context · {}",                                                             "LLM 上下文 · {}"                                                                                      },
    {"ctx.empty",                       "( empty )",                                                                     "( 空 )"                                                                                                  },
    {"ctx.hint",
     " [Click/Enter/Space] Toggle [Wheel/Up/Down] Scroll [PgUp/PgDn] Page [Esc] Close ",                                 " [点击/Enter/空格] 展开或折叠 [滚轮/方向键] 滚动 [PgUp/PgDn] 翻页 [Esc] 关闭 "        },

 // ---- Mermaid 状态图弹窗 ----
    {"graph.title",                     "Graph",                                                                         "状态图"                                                                                                },
    {"graph.noDiagram",                 "( no diagram )",                                                                "( 无状态图 )"                                                                                         },

 // ---- 通用滚动弹窗提示 (Mermaid/加载失败共用) ----
    {"overlay.scrollHint",              " [Wheel/Up/Down] Scroll [Esc] Close ",                                          " [滚轮/方向键] 滚动 [Esc] 关闭 "
    },

 // ---- 加载失败组件弹窗 ----
    {"failed.title",                    "Failed Components",                                                             "加载失败的组件"                                                                                    },
    {"failed.empty",                    "( no failed components )",                                                      "( 无失败组件 )"                                                                                      },
    {"failed.unknownType",              "Unknown",                                                                       "未知"                                                                                                   },

 // ---- 中断表单控件 ----
    {"interrupt.header",                "! [Interrupt] ",                                                                "! [中断] "                                                                                              },
    {"interrupt.permissionBadge",       "! [Permission] ",                                                               "! [权限] "                                                                                              },
    {"interrupt.noDescriptor",          "! [Interrupt] missing UI descriptor",                                           "! [中断] 缺少 UI 描述"                                                                              },
    {"interrupt.confirmed",             "Confirmed: {}",                                                                 "已确认: {}"                                                                                            },
    {"interrupt.confirmedEmpty",        "Confirmed",                                                                     "已确认"                                                                                                },
    {"interrupt.cancelled",             "Cancelled",                                                                     "已取消"                                                                                                },
    {"interrupt.expired",               "Expired",                                                                       "已过期"                                                                                                },
    {"interrupt.yes",                   "Yes",                                                                           "是"                                                                                                      },
    {"interrupt.no",                    "No",                                                                            "否"                                                                                                      },
    {"interrupt.confirm",               "Confirm",                                                                       "确认"                                                                                                   },
    {"interrupt.cancel",                "✕",                                                                           "✕"                                                                                                      },
    {"interrupt.allow",                 "Allow",                                                                         "允许"                                                                                                   },
    {"interrupt.deny",                  "Deny",                                                                          "拒绝"                                                                                                   },
    {"interrupt.remember",              "Remember this choice",                                                          "记住此选择"                                                                                          },
    {"interrupt.rememberDir",
     "• Will also authorize its subdirectories and files",                                                             "• 且授权子目录与文件"                                                                          },
    {"interrupt.fullAuth",              "Fully authorize all permissions",                                               "完全授权所有权限"                                                                                 },
    {"interrupt.tipInt",                "( Invalid integer, please input again. )",                                      "( 无效整数, 请重新输入。 )"                                                                     },
    {"interrupt.tipNum",                "( Invalid number, please input again. )",                                       "( 无效数字, 请重新输入。 )"                                                                     },
    {"interrupt.tipRange",
     "( Out of range (limit: {}), please input again. )",                                                                "( 超出范围 (限制: {}), 请重新输入。 )"                                                        },
 // ---- 通用 UI 组件 (插件面板 / overlay / 中断 共用文案) ----
    {"ui.submit",                       "Submit",                                                                        "提交"                                                                                                   },
    {"ui.save",                         "Save",                                                                          "保存"                                                                                                   },
    {"ui.cancel",                       "Cancel",                                                                        "取消"                                                                                                   },
    {"ui.none",                         "(none)",                                                                        "(无)"                                                                                                    },
    {"ui.noOptions",                    "( No options available, cannot submit. )",                                      "( 没有可选项, 无法提交。 )"                                                                     },
    {"ui.invalidNumber",                "( Invalid number, please input again. )",                                       "( 无效数字, 请重新输入。 )"                                                                     },
    {"ui.invalidInteger",               "( Invalid integer, please input again. )",                                      "( 无效整数, 请重新输入。 )"                                                                     },
    {"ui.outOfRange",
     "( Out of range (limit: {}), please input again. )",                                                                "( 超出范围 (限制: {}), 请重新输入。 )"                                                        },
    {"interrupt.tipNoOptions",
     "( This control has no selectable option. )",                                                                       "( 该控件没有可选候选项。 )"                                                                    },

 // ---- 思考消息 (加密思考占位) ----
    {"think.encryptedTokens",           "encrypted thinking {} tokens",                                                  "加密思考 {} 词元"                                                                                   },
    {"think.encrypted",                 "Thinking content is encrypted",                                                 "思考内容被加密"                                                                                    },

 // ---- 消息列表角色标签 (随语言切换; 值自带首尾空格, 直接拼在 1 列折叠标记后;
  //      列宽预算经 markdown::utf8_display_width 计算, 不按英文宽度写死) ----
    {"msg.roleSystem",                  " [System] ",                                                                    " [系统] "                                                                                               },
    {"msg.roleThink",                   " [Think] ",                                                                     " [思考] "                                                                                               },
    {"msg.roleTool",                    " [Tool] ",                                                                      " [工具] "                                                                                               },
    {"msg.tipPrefix",                   " [Tip] # {}",                                                                   " [提示] # {}"                                                                                           },
    {"msg.tipLevelInfo",                "Info",                                                                          "信息"                                                                                                   },
    {"msg.tipLevelWarn",                "Warn",                                                                          "警告"                                                                                                   },
    {"msg.tipLevelError",               "Error",                                                                         "错误"                                                                                                   },

 // ---- 工具消息正文标签 (字段前缀, 随语言切换) ----
    {"tool.args",                       "  args: ",                                                                      "  参数: "                                                                                               },
    {"tool.result",                     "  result: ",                                                                    "  结果: "                                                                                               },
    {"tool.file",                       "  file: ",                                                                      "  文件: "                                                                                               },
    {"tool.running",                    "  running...",                                                                  "  运行中..."                                                                                           },
    {"tool.noChanges",                  "  ( no changes )",                                                              "  ( 未更改 )"                                                                                          },

 // ---- 状态栏 ----
    {"status.modelNone",                "( none )",                                                                      "( 未选择 )"                                                                                            },
    {"status.sessions",                 "[F3] Sessions",                                                                 "[F3] 会话"                                                                                              },
    {"status.settings",                 "[F4] Settings",                                                                 "[F4] 设置"                                                                                              },

 // ---- 侧边栏 (Info/Logs 常驻标签) ----
    {"sidebar.info",                    "Info",                                                                          "信息"                                                                                                   },
    {"sidebar.logs",                    "Logs",                                                                          "日志"                                                                                                   },

 // ---- 日志/Info 侧边栏内容 ----
    {"info.empty",                      "( Empty )",                                                                     "( 空 )"                                                                                                  },
    {"info.append",                     "Append",                                                                        "已加载"                                                                                                },
    {"info.appendFailed",               "Failed: {}",                                                                    "失败: {}"                                                                                               },
    {"info.viewFailed",                 "[ View ]",                                                                      "[ 查看 ]"                                                                                               },
    {"info.innerServer",                "Inner Server",                                                                  "内置服务"                                                                                             },
    {"info.workDirUnknown",             "( Unknown Work Dir )",                                                          "( 未知工作目录 )"                                                                                   },
 // 授权状态按钮 (Info 侧边栏底部工作目录行): 显示当前状态, 点击切换
    {"info.authFull",                   "[ Full Permission ]",                                                           "[ 完全授权 ]"                                                                                         },
    {"info.authAsk",                    "[ Ask Permission ]",                                                            "[ 询问授权 ]"                                                                                         },
 // 更新提示行 (启动检查发现新版本时; 点击复制发布页链接)
    {"info.updateNotice",               "[ New Version {} ]",                                                            "[ 新版本 {} ]"                                                                                         },
    {"info.updateHint",                 "click to copy the release link",                                                "点击复制发布链接"                                                                                 },
    {"info.idle",                       "idle",                                                                          "空闲"                                                                                                   },
    {"footer.menu",                     "[ Menu ]",                                                                      "[ 菜单 ]"                                                                                               },
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

} // namespace agentxx::client
