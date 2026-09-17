#pragma once

/// pluginxx 版本标识
///
/// - 版本号由构建侧经宏 `XX_VERSION_STRING` 注入 (superbuild 传入项目版本号);
///   未注入时回退 "0.1.0"
/// - 注意: 插件框架自身的**跨边界契约版本**是 `AGENTXX_PLUGIN_API_VERSION`
///   (C ABI 宏, 定义在 pluginxx/api/abi.h), 与本软件发行版本无关

#include <string_view>

namespace pluginxx {

/// 返回本库的版本字符串视图
[[nodiscard]] std::string_view version() noexcept;

} // namespace pluginxx
