#pragma once

#include "agentxx/agent/config.h"
#include <expected>
#include <set>
#include <string>
#include <string_view>

namespace agentxx {
namespace agent {

/// yaml 配置文件名 (与客户端配置加载的默认文件名一致, 见
/// agentxx-client/config_loader.h 的 kDefaultConfigFileName)
inline constexpr std::string_view kConfigYamlFileName = "agentxx-config.yaml";

/// 校验"新增模型配置"的取值 (客户端表单与服务端注册共用同一套规则)
///
/// 校验项:
/// - 名称: 非空, 无首尾空白, 不含换行, 且不在 `existingNames` 中 (重名拒绝)
/// - 类型: 空串按 `openai` 处理; 否则必须是 openai / openai-responses / anthropic
/// - 地址与密钥: 至少给出其一 (两者都缺省的配置无法发起请求);
///   地址非空时必须以 http:// 或 https:// 开头
/// - 数值范围: 连接/读取超时 [1, 86400] 秒; 最大并发连接 [0, 4096] (0 = 不限制);
///   上下文 token 上限 [0, 10000000] (0 = 未指定)
/// - `extraConfig` / `extraHeaders`: 非空时必须是对象 (前者为任意 JSON 对象,
///   后者为 字符串→字符串 的对象)
///
/// - `args`:
///     - [mc] 待校验的模型配置
///     - [existingNames] 已存在的模型名称集合 (来自当前可用模型列表)
/// - `return` 通过返回空 expected; 失败返回给用户看的错误文本
std::expected<void, std::string> validateNewModelConfig(
    const ModelConfig&                                mc,
    const std::set<std::string, std::less<>>&         existingNames
);

/// 把模型配置追加到 yaml 文件的 `model.list` 段 (文件不存在时创建)
///
/// 写入方式为**按行文本插入**, 不用 yaml-cpp 重排整个文件: 用户配置里的注释、
/// 键顺序、缩进风格都原样保留, 只在 `model.list` 的最后一个条目之后插入新条目
/// (段结构说明见 agentxx-config.yaml 模板与 agentxx-client/config_loader.h)。
///
/// 处理的结构 (其余形态按"不支持的配置结构"报错, 不改动文件):
/// - 文件不存在/为空: 新建 `model: / list:` 段并写入条目
/// - 根节点有 `model:` 映射, 且其中有块序列 `list:`: 在最后一个条目之后追加
/// - 根节点有 `model:` 映射但没有 `list:`: 追加 `list:` 行后写入条目
/// - 根节点没有 `model:`: 在文件末尾追加 `model: / list:` 段
/// - `list: []` (空流式序列) 会改写为块序列后再追加
///
/// 安全性:
/// - 同名的 `model.list` 条目已存在时返回错误 (不覆盖已有配置)
/// - 写入前先按新内容在内存中重新解析校验 (条目存在且关键字段一致),
///   校验不通过则不写文件, 因此不存在"写坏用户配置"的路径
/// - 行尾风格跟随原文件 (原文件用 CRLF 时插入行也用 CRLF)
///
/// - `args`:
///     - [yamlPath] yaml 配置文件路径 (UTF-8; 父目录不存在时自动创建)
///     - [mc] 待写入的模型配置 (名称为空时返回错误)
/// - `return` 成功返回空 expected; 失败返回错误描述 (文件保持原样)
std::expected<void, std::string>
    appendModelConfigToYamlFile(std::string_view yamlPath, const ModelConfig& mc);

/// 从 yaml 文件的 `model.list` 段删除指定模型条目
///
/// 与 [appendModelConfigToYamlFile] 对称: 同样按行文本删除, 保留其余条目的注释、
/// 键顺序与缩进风格; 写入前回读校验"目标条目已消失且条目数只减 1", 校验不通过
/// 则不写文件。
///
/// 删除范围: 条目自身的全部行, 以及紧邻其上方的"由界面添加"说明注释 (其余注释保留)。
/// 只操作 [yamlPath] 指向的那一个文件 (overlay 层里的同名条目由调用方自行处理)。
///
/// - `args`:
///     - [yamlPath] yaml 配置文件路径 (UTF-8)
///     - [modelName] 待删除的模型名称 (与 `name` 字段精确匹配)
///
/// - `return`:
///     - `true`:  文件里原本有该条目, 已删除
///     - `false`: 文件里没有该条目 (文件不存在/内容为空/条目缺失), 无需改动 ——
///       不算错误: 模型可能来自 overlay 层配置或运行时注入, 调用方只摘除运行时即可
///     - `unexpected`: 读不了文件 / 配置结构不支持 / 写盘失败 (文件保持原样)
std::expected<bool, std::string>
    removeModelConfigFromYamlFile(std::string_view yamlPath, std::string_view modelName);

/// 模型配置对应的 yaml 配置文件路径: {dataDir}/agentxx-config.yaml
/// - dataDir 为空时取系统数据目录 (与设置库/会话数据的回退一致,
///   见 AgentConfigStatic::getDataDir)
std::string modelConfigYamlPath(std::string_view dataDir);

} // namespace agent
} // namespace agentxx
