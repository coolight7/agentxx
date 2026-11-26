# JS 示例插件 (纯脚本插件: 没有动态库, 只装清单与脚本)
# - 清单写 `kind: js`, 主程序把插件目录交给 `js` 形态的运行器 (引擎插件) 执行
#   `plugin.js`, 不再需要 C++ 壳;
# - 构建只做一件事: 把 plugin.yaml 与 plugin.js 拷到运行期插件目录
#   `${AGENTXX_EXEC_INSTALL_PREFIX}/plugins/example_js/`;
# - 内置合并编译模式下同样只拷资源: 脚本插件本来就没有可合并的库。
set(PLUGIN_NAME example_js)

set(_dest_dir "${AGENTXX_EXEC_INSTALL_PREFIX}/plugins/${PLUGIN_NAME}")
set(_manifest "${_dest_dir}/plugin.yaml")
set(_script "${_dest_dir}/plugin.js")

# 资源拷贝用自包含 ALL 目标: 跨目录 OUTPUT 不进构建图 (见 plugins.md),
# 这样独立构建与内置合并两种模式都随构建执行。
add_custom_target(${PLUGIN_NAME}_resources ALL
  COMMAND ${CMAKE_COMMAND} -E make_directory "${_dest_dir}"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_CURRENT_SOURCE_DIR}/plugin.yaml" "${_manifest}"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_CURRENT_SOURCE_DIR}/plugin.js" "${_script}"
  DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/plugin.yaml"
    "${CMAKE_CURRENT_SOURCE_DIR}/plugin.js"
  COMMENT "Copy ${PLUGIN_NAME} plugin resources to ${_dest_dir}")
