#include "agentxx/agent/config_writer.h"

#include "agentxx/agent/config_static.h"
#include "fmt/format.h"
#include "utilxx_base/json.h"
#include "utilxx_base/log.h"
#include "utilxx_base/string_util.h"
#include "yaml-cpp/yaml.h"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

namespace agentxx {
namespace agent {

namespace {

// ---------------------------------------------------------------------------
// 文本与 YAML 取值辅助
// ---------------------------------------------------------------------------

/// 新条目上方的说明注释
constexpr std::string_view kEntryComment = "# 由界面添加, 可直接编辑";

/// 新建文件时的头部说明
constexpr std::string_view kNewFileHeader
    = "# Agentxx 配置 (数据目录下的 base 层): 界面添加的模型配置写入本文件\n"
      "# 工作目录 (或 --config 指定) 的 agentxx-config.yaml 为 overlay 层, 覆盖本文件\n";

static bool isBlankChar(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

/// 去掉首尾空白 (空格/制表/换行)
static std::string trimWhitespace(std::string_view s) {
    size_t begin = 0;
    size_t end   = s.size();
    while (begin < end && isBlankChar(s[begin])) {
        ++begin;
    }
    while (end > begin && isBlankChar(s[end - 1])) {
        --end;
    }
    return std::string{s.substr(begin, end - begin)};
}

/// 行首缩进的空格列数 (制表符按 1 列计; 配置模板只用空格)
static int indentOf(std::string_view line) {
    int n = 0;
    for (char c : line) {
        if (c == ' ' || c == '\t') {
            ++n;
        } else {
            break;
        }
    }
    return n;
}

/// 行首去空白后的内容
static std::string_view trimmedView(std::string_view line) {
    size_t i = 0;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) {
        ++i;
    }
    return line.substr(i);
}

/// 空行或整行注释
static bool isBlankOrComment(std::string_view line) {
    auto t = trimmedView(line);
    return t.empty() || t.starts_with('#');
}

/// 去掉行尾注释 (`#` 在行首或前面是空白时才算注释, 与 YAML 规则一致)
static std::string_view stripLineComment(std::string_view text) {
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '#') {
            if (i == 0 || text[i - 1] == ' ' || text[i - 1] == '\t') {
                return text.substr(0, i);
            }
        }
    }
    return text;
}

/// 根级键行判定 (列 0 且以 `key:` 开头)
static bool isRootKeyLine(std::string_view line, std::string_view key) {
    if (indentOf(line) != 0) {
        return false;
    }
    auto t = trimmedView(line);
    if (!t.starts_with(key)) {
        return false;
    }
    return t.size() > key.size() && t[key.size()] == ':';
}

/// `key:` 之后的内联值 (已去掉行尾注释; 块写法返回空串)
static std::string inlineValueOf(std::string_view line, std::string_view key) {
    auto t = trimmedView(line);
    if (!t.starts_with(key) || t.size() <= key.size() || t[key.size()] != ':') {
        return {};
    }
    return trimWhitespace(stripLineComment(t.substr(key.size() + 1)));
}

/// yaml 双引号字符串 (转义反斜杠/引号/控制字符)
static std::string yamlQuoted(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('"');
    for (char c : s) {
        switch (c) {
            case '\\':
                out += "\\\\";
                break;
            case '"':
                out += "\\\"";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                out.push_back(c);
                break;
        }
    }
    out.push_back('"');
    return out;
}

/// yaml 键名: 简单标识符按原样, 其余加引号 (键来自用户输入, 如自定义请求头)
static std::string yamlKeyText(std::string_view k) {
    if (k.empty()) {
        return "\"\"";
    }
    const bool simple = (std::isalpha(static_cast<unsigned char>(k[0])) || k[0] == '_')
                        && std::all_of(k.begin(), k.end(), [](char c) {
                               return std::isalnum(static_cast<unsigned char>(c)) || c == '_'
                                      || c == '.' || c == '-';
                           });
    return simple ? std::string{k} : yamlQuoted(k);
}

/// JSON 标量 → yaml 文本
static std::string yamlScalarOf(const utilxx_base::Json& v) {
    if (v.is_string()) {
        return yamlQuoted(v.get<std::string>());
    }
    if (v.is_boolean()) {
        return v.get<bool>() ? "true" : "false";
    }
    if (v.is_null()) {
        return "null";
    }
    // 数值与空对象/空数组: 直接用 JSON 文本 (yaml 都接受)
    return v.dump();
}

/// JSON 值是带内容的复合结构 (对象/数组且非空)
static bool isBlockJson(const utilxx_base::Json& v) {
    return (v.is_object() || v.is_array()) && v.size() > 0;
}

/// JSON 值 → yaml 行 (递归; 嵌套块缩进逐层 +2)
static void appendJsonValueLines(const utilxx_base::Json& v, int indent, std::vector<std::string>& out) {
    const std::string pad(static_cast<size_t>(indent), ' ');
    if (v.is_object()) {
        for (const auto& [k, child] : v.items()) {
            if (isBlockJson(child)) {
                out.push_back(pad + yamlKeyText(k) + ":");
                appendJsonValueLines(child, indent + 2, out);
            } else {
                out.push_back(pad + yamlKeyText(k) + ": " + yamlScalarOf(child));
            }
        }
        return;
    }
    if (v.is_array()) {
        for (const auto& child : v) {
            if (isBlockJson(child)) {
                out.push_back(pad + "-");
                appendJsonValueLines(child, indent + 2, out);
            } else {
                out.push_back(pad + "- " + yamlScalarOf(child));
            }
        }
        return;
    }
    out.push_back(pad + yamlScalarOf(v));
}

/// 模型配置 → yaml 条目行 (`- name: ...` 开头, 其余键相对条目缩进 2 列)
static std::vector<std::string> modelEntryLines(const ModelConfig& mc, int itemIndent) {
    const std::string pad(static_cast<size_t>(itemIndent), ' ');
    const std::string keyPad(static_cast<size_t>(itemIndent + 2), ' ');
    const ModelConfig& defaults = ModelConfig::defaultModelConfig;

    std::vector<std::string> out;
    out.push_back(pad + std::string{kEntryComment});
    out.push_back(pad + "- name: " + yamlQuoted(mc.name));
    out.push_back(keyPad + "type: " + yamlQuoted(mc.type.empty() ? std::string{"openai"} : mc.type));
    out.push_back(keyPad + "base_url: " + yamlQuoted(mc.baseUrl));
    if (!mc.apiPath.empty()) {
        out.push_back(keyPad + "api_path: " + yamlQuoted(mc.apiPath));
    }
    out.push_back(keyPad + "api_key: " + yamlQuoted(mc.apiKey.empty() ? "EMPTY" : mc.apiKey));
    if (!mc.modelName.empty()) {
        out.push_back(keyPad + "model_name: " + yamlQuoted(mc.modelName));
    }
    if (mc.modelContextMaxToken > 0) {
        out.push_back(keyPad + fmt::format("model_context_max_token: {}", mc.modelContextMaxToken));
    }
    if (mc.sendThinking) {
        out.push_back(keyPad + "send_thinking: true");
    }
    if (!mc.requestReasoningSummary) {
        out.push_back(keyPad + "request_reasoning_summary: false");
    }
    if (mc.connectTimeoutSeconds != defaults.connectTimeoutSeconds) {
        out.push_back(keyPad + fmt::format("connect_timeout: {}", mc.connectTimeoutSeconds));
    }
    if (mc.readChunkTimeoutSeconds != defaults.readChunkTimeoutSeconds) {
        out.push_back(keyPad + fmt::format("read_chunk_timeout: {}", mc.readChunkTimeoutSeconds));
    }
    if (mc.sslVerify.has_value()) {
        out.push_back(keyPad + fmt::format("ssl_verify: {}", *mc.sslVerify ? "true" : "false"));
    }
    if (mc.maxConcurrentConnections != defaults.maxConcurrentConnections) {
        out.push_back(
            keyPad + fmt::format("max_concurrent_connections: {}", mc.maxConcurrentConnections)
        );
    }
    if (mc.imageInput) {
        out.push_back(keyPad + "image_input: true");
    }
    if (mc.audioInput) {
        out.push_back(keyPad + "audio_input: true");
    }
    if (mc.videoInput) {
        out.push_back(keyPad + "video_input: true");
    }
    if (!mc.extraHeaders.empty()) {
        utilxx_base::Json headers = utilxx_base::Json::object();
        for (const auto& [k, v] : mc.extraHeaders) {
            headers[k] = v;
        }
        out.push_back(keyPad + "extra_headers:");
        appendJsonValueLines(headers, itemIndent + 4, out);
    }
    if (isBlockJson(mc.extraConfig)) {
        out.push_back(keyPad + "extra_api_config:");
        appendJsonValueLines(mc.extraConfig, itemIndent + 4, out);
    }
    return out;
}

// ---------------------------------------------------------------------------
// 文件内容 <-> 行
// ---------------------------------------------------------------------------

/// 按行切分 (识别原文件行尾风格; 结尾换行不产生空行)
static std::vector<std::string> splitLines(std::string_view text, std::string& eol) {
    eol = (text.find("\r\n") != std::string_view::npos) ? "\r\n" : "\n";
    std::string norm{text};
    if (eol == "\r\n") {
        std::string stripped;
        stripped.reserve(norm.size());
        for (size_t i = 0; i < norm.size(); ++i) {
            if (norm[i] == '\r' && i + 1 < norm.size() && norm[i + 1] == '\n') {
                continue;
            }
            stripped.push_back(norm[i]);
        }
        norm = std::move(stripped);
    }
    std::vector<std::string> lines;
    size_t                   start = 0;
    for (;;) {
        auto pos = norm.find('\n', start);
        if (pos == std::string::npos) {
            lines.push_back(norm.substr(start));
            break;
        }
        lines.push_back(norm.substr(start, pos - start));
        start = pos + 1;
    }
    if (!lines.empty() && lines.back().empty()) {
        lines.pop_back();
    }
    return lines;
}

static std::string joinLines(const std::vector<std::string>& lines, std::string_view eol) {
    std::string out;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i > 0) {
            out += eol;
        }
        out += lines[i];
    }
    return out;
}

/// 读整个文件 (二进制; 读不到返回 nullopt)
static std::optional<std::string> readFileText(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    std::string out;
    in.seekg(0, std::ios::end);
    const auto size = in.tellg();
    if (size > 0) {
        out.resize(static_cast<size_t>(size));
    }
    in.seekg(0, std::ios::beg);
    if (!out.empty()) {
        in.read(out.data(), static_cast<std::streamsize>(out.size()));
    }
    if (in.bad()) {
        return std::nullopt;
    }
    return out;
}

/// yaml 子键 (缺键/非映射返回空节点; 不改动原节点)
static YAML::Node mapChild(const YAML::Node& node, std::string_view key) {
    if (!node || !node.IsMap()) {
        return {};
    }
    auto child = node[std::string{key}];
    return (child && !child.IsNull()) ? child : YAML::Node{};
}

// ---------------------------------------------------------------------------
// 写入后的校验
// ---------------------------------------------------------------------------

/// 按新内容重新解析, 确认目标条目的关键字段与写入意图一致
static std::expected<void, std::string> verifyNewText(std::string_view newText, const ModelConfig& mc) {
    YAML::Node root;
    try {
        root = YAML::Load(std::string{newText});
    } catch (const std::exception& e) {
        return std::unexpected{fmt::format("写入后解析校验失败: {}", e.what())};
    }
    const auto listNode = mapChild(mapChild(root, "model"), "list");
    if (!listNode.IsSequence()) {
        return std::unexpected{"写入后校验失败: 未找到 model.list 段"};
    }
    for (const auto& item : listNode) {
        if (!item.IsMap() || item["name"].as<std::string>("") != mc.name) {
            continue;
        }
        if (item["type"].as<std::string>("")
            != (mc.type.empty() ? std::string{"openai"} : mc.type)) {
            return std::unexpected{"写入后校验失败: 模型类型不一致"};
        }
        if (item["base_url"].as<std::string>("") != mc.baseUrl) {
            return std::unexpected{"写入后校验失败: API 地址不一致"};
        }
        if (item["api_key"].as<std::string>("") != (mc.apiKey.empty() ? "EMPTY" : mc.apiKey)) {
            return std::unexpected{"写入后校验失败: API Key 不一致"};
        }
        if (!mc.modelName.empty() && item["model_name"].as<std::string>("") != mc.modelName) {
            return std::unexpected{"写入后校验失败: 模型名不一致"};
        }
        if (isBlockJson(mc.extraConfig)) {
            const auto extra = item["extra_api_config"];
            if (!extra || !extra.IsMap()) {
                return std::unexpected{"写入后校验失败: extra_api_config 不是映射"};
            }
        }
        return {};
    }
    return std::unexpected{fmt::format("写入后校验失败: 未找到模型 '{}'", mc.name)};
}

/// 条目行区间 → 可解析的 yaml 映射文本 (删除前判定"这一条是不是目标模型")
/// - 内容列统一为 `itemIndent + 2`: 该列之前的空白 (首行还有 `- ` 前缀) 全部去掉,
///   于是各键在还原后的文本里回到同一层缩进, 拼出来的是一份合法映射
///   (只去 `itemIndent` 会让首行在第 0 列、其余键在第 2 列, yaml 会把后续键
///   当成首行标量的多行续写, name 取不到值)
static std::string
    entryMappingText(const std::vector<std::string>& lines, size_t begin, size_t end, int itemIndent) {
    const size_t stripCols = static_cast<size_t>(itemIndent) + 2;
    std::string  out;
    for (size_t i = begin; i < end; ++i) {
        std::string_view lv{lines[i]};
        size_t           skip = 0;
        while (skip < lv.size() && (lv[skip] == ' ' || lv[skip] == '\t') && skip < stripCols) {
            ++skip;
        }
        if (i == begin && lv.substr(skip).starts_with("- ")) {
            skip += 2;
        }
        out.append(lv.substr(skip));
        out.push_back('\n');
    }
    return out;
}

/// model.list 里的一个条目 (纯行扫描结果)
struct ModelListEntry {
    size_t      begin = 0; ///< 条目起始行 (`- ` 开头那一行)
    size_t      end   = 0; ///< 条目结束行 (不含: 下一条目起始行或 list 块之后)
    std::string name;      ///< 解析出的 name (单条解析失败时为空串)
};

/// 扫描 yaml 行里的 model.list 条目
///
/// 只做行扫描 (不依赖 yaml 节点类型判定): 追加、删除与删除后回读校验共用同一套
/// 结构假设, 三者对"条目/注释/空行"的认定天然一致。
///
/// - `args`:
///     - [lines] 文件按行切分的结果 (见 [splitLines])
///     - [err] 结构不支持时写入原因
/// - `return` 条目列表 (可为空); 结构不支持 (缺 model/list 段、内联写法、
///   条目区出现非条目内容) 返回 nullopt
static std::optional<std::vector<ModelListEntry>>
    scanModelListEntries(const std::vector<std::string>& lines, std::string& err) {
    err.clear();
    size_t modelLine = lines.size();
    for (size_t i = 0; i < lines.size(); ++i) {
        if (isRootKeyLine(lines[i], "model")) {
            modelLine = i;
            break;
        }
    }
    if (modelLine == lines.size()) {
        err = "未找到 model 段";
        return std::nullopt;
    }
    // 段范围: 到下一个根级键为止 (空行不结束段)
    size_t sectionEnd = lines.size();
    for (size_t i = modelLine + 1; i < lines.size(); ++i) {
        if (!lines[i].empty() && indentOf(lines[i]) == 0) {
            sectionEnd = i;
            break;
        }
    }
    // `list:` 行 (取缩进最小的那个)
    size_t listLine   = sectionEnd;
    int    listIndent = 0;
    for (size_t i = modelLine + 1; i < sectionEnd; ++i) {
        if (lines[i].empty()) {
            continue;
        }
        const int  ind = indentOf(lines[i]);
        const auto t   = trimmedView(lines[i]);
        if (ind <= 0 || !t.starts_with("list") || t.size() <= 4 || t[4] != ':') {
            continue;
        }
        if (listLine == sectionEnd || ind < listIndent) {
            listLine   = i;
            listIndent = ind;
        }
    }
    if (listLine == sectionEnd) {
        err = "model 段没有 list 列表";
        return std::nullopt;
    }
    const std::string inlineVal = inlineValueOf(lines[listLine], "list");
    if (inlineVal == "[]") {
        return std::vector<ModelListEntry>{}; // 空流式序列: 没有条目
    }
    if (!inlineVal.empty()) {
        err = "model.list 是内联写法";
        return std::nullopt;
    }

    std::vector<ModelListEntry> entries;
    for (size_t i = listLine + 1; i < sectionEnd;) {
        if (lines[i].empty() || isBlankOrComment(lines[i])) {
            ++i;
            continue;
        }
        if (indentOf(lines[i]) <= listIndent) {
            break; // 回到 list 的同级/更外层: 条目区结束
        }
        const int startIndent = indentOf(lines[i]);
        if (!trimmedView(lines[i]).starts_with('-')) {
            err = "model.list 下存在非条目内容";
            return std::nullopt;
        }
        size_t end = i + 1;
        for (; end < sectionEnd; ++end) {
            if (lines[end].empty() || isBlankOrComment(lines[end])) {
                continue;
            }
            if (indentOf(lines[end]) <= listIndent) {
                break;
            }
            // 同缩进的 `- ` 起始行 = 下一个条目
            if (indentOf(lines[end]) == startIndent && trimmedView(lines[end]).starts_with('-')) {
                break;
            }
        }
        ModelListEntry entry;
        entry.begin = i;
        entry.end   = end;
        try {
            const auto node = YAML::Load(entryMappingText(lines, i, end, startIndent));
            if (node.IsMap()) {
                entry.name = node["name"].as<std::string>("");
            }
        } catch (const std::exception&) {
            // 单条解析不了: name 留空 (既不会误删, 也不影响其它条目)
        }
        entries.push_back(std::move(entry));
        i = end;
    }
    return entries;
}

/// 原子写文本: 先写临时文件再改名; 失败时回写 [original] 并报错
static std::expected<void, std::string> writeTextAtomically(
    const std::filesystem::path& path,
    std::string_view             newText,
    std::string_view             original
) {
    std::error_code ec;
    const auto      dir = path.parent_path();
    if (!dir.empty()) {
        std::filesystem::create_directories(dir, ec);
        if (ec) {
            return std::unexpected{fmt::format("创建目录失败: {}", ec.message())};
        }
    }
    auto tmp = path;
    tmp += ".tmp";
    {
        std::ofstream outFile(tmp, std::ios::binary | std::ios::trunc);
        if (!outFile) {
            return std::unexpected{
                fmt::format("无法写入临时文件: {}", utilxx_base::pathToUtf8Generic(tmp))
            };
        }
        outFile.write(newText.data(), static_cast<std::streamsize>(newText.size()));
        outFile.flush();
        if (!outFile) {
            outFile.close();
            std::filesystem::remove(tmp, ec);
            return std::unexpected{"写入配置内容失败"};
        }
    }
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        // Windows 下目标已存在时改名会失败: 先删除目标再改名, 改名仍失败则回写原内容
        std::error_code removeEc;
        std::filesystem::remove(path, removeEc);
        std::filesystem::rename(tmp, path, ec);
        if (ec) {
            std::error_code tmpEc;
            std::filesystem::remove(tmp, tmpEc);
            if (!original.empty()) {
                std::ofstream restore(path, std::ios::binary | std::ios::trunc);
                restore.write(original.data(), static_cast<std::streamsize>(original.size()));
            }
            return std::unexpected{fmt::format("保存配置文件失败: {}", ec.message())};
        }
    }
    return {};
}

} // namespace

// ---------------------------------------------------------------------------
// 校验
// ---------------------------------------------------------------------------

std::expected<void, std::string>
    validateNewModelConfig(const ModelConfig& mc, const std::set<std::string, std::less<>>& existingNames) {
    if (trimWhitespace(mc.name).empty()) {
        return std::unexpected{"模型名称不能为空"};
    }
    if (trimWhitespace(mc.name) != mc.name) {
        return std::unexpected{"模型名称不能包含首尾空白"};
    }
    if (mc.name.find('\n') != std::string::npos || mc.name.find('\r') != std::string::npos) {
        return std::unexpected{"模型名称不能包含换行"};
    }
    if (existingNames.contains(mc.name)) {
        return std::unexpected{fmt::format("模型名称已存在: {}", mc.name)};
    }

    const std::string type = mc.type.empty() ? std::string{"openai"} : mc.type;
    if (type != "openai" && type != "openai-responses" && type != "anthropic") {
        return std::unexpected{
            fmt::format("不支持的模型类型: {} (可选 openai / openai-responses / anthropic)", type)
        };
    }

    if (mc.baseUrl.empty() && (mc.apiKey.empty() || mc.apiKey == "EMPTY")) {
        return std::unexpected{"API 地址与 API Key 至少填写一项 (无鉴权服务的 Key 可填 EMPTY)"};
    }
    if (!mc.baseUrl.empty() && !mc.baseUrl.starts_with("http://")
        && !mc.baseUrl.starts_with("https://")) {
        return std::unexpected{"API 地址应以 http:// 或 https:// 开头"};
    }

    if (mc.connectTimeoutSeconds < 1 || mc.connectTimeoutSeconds > 86400) {
        return std::unexpected{"连接超时应在 1 ~ 86400 秒之间"};
    }
    if (mc.readChunkTimeoutSeconds < 1 || mc.readChunkTimeoutSeconds > 86400) {
        return std::unexpected{"读取超时应 1 ~ 86400 秒之间"};
    }
    if (mc.maxConcurrentConnections > 4096) {
        return std::unexpected{"最大并发连接数应不超过 4096 (0 = 不限制)"};
    }
    if (mc.modelContextMaxToken > 10000000) {
        return std::unexpected{"上下文 token 上限应不超过 10000000 (0 = 未指定)"};
    }

    if (!mc.extraConfig.is_null() && !mc.extraConfig.is_object()) {
        return std::unexpected{"额外 API 参数 (extra_api_config) 必须是 JSON 对象"};
    }
    for (const auto& [k, v] : mc.extraHeaders) {
        (void)v;
        if (trimWhitespace(k).empty()) {
            return std::unexpected{"额外请求头的名称不能为空"};
        }
    }
    return {};
}

std::string modelConfigYamlPath(std::string_view dataDir) {
    return (std::filesystem::path(AgentConfigStatic::getDataDir(dataDir))
            / std::string{kConfigYamlFileName})
        .string();
}

// ---------------------------------------------------------------------------
// 追加写入
// ---------------------------------------------------------------------------

std::expected<void, std::string>
    appendModelConfigToYamlFile(std::string_view yamlPath, const ModelConfig& mc) {
    if (yamlPath.empty()) {
        return std::unexpected{"配置路径为空"};
    }
    if (trimWhitespace(mc.name).empty()) {
        return std::unexpected{"模型名称不能为空"};
    }
    const auto  path = utilxx_base::utf8ToPath(yamlPath);
    std::string original; ///< 原文件内容 (改名失败时用于回滚)
    {
        std::error_code ec;
        const bool      exists = std::filesystem::exists(path, ec);
        if (ec) {
            return std::unexpected{fmt::format("无法访问配置文件: {}", ec.message())};
        }
        if (exists) {
            auto text = readFileText(path);
            if (!text.has_value()) {
                return std::unexpected{"读取配置文件失败"};
            }
            original = std::move(*text);
        }
    }

    // 已有内容先解析一遍: 重名检查与整体合法性 (解析失败说明文件本身有问题, 不动它)
    const bool hasContent = !trimWhitespace(original).empty();
    if (hasContent) {
        YAML::Node root;
        try {
            root = YAML::Load(original);
        } catch (const std::exception& e) {
            return std::unexpected{fmt::format("配置文件解析失败: {}", e.what())};
        }
        const auto listNode = mapChild(mapChild(root, "model"), "list");
        if (listNode.IsSequence()) {
            for (const auto& item : listNode) {
                if (item.IsMap() && item["name"].as<std::string>("") == mc.name) {
                    return std::unexpected{fmt::format("配置文件里已有同名模型: {}", mc.name)};
                }
            }
        }
    }

    // ---- 计算插入位置 (纯文本插入: 保留原文件的注释、键顺序与缩进风格) ----
    std::string              eol;
    std::vector<std::string> lines       = splitLines(original, eol);
    const bool               endsWithEol = !original.empty() && original.back() == '\n';

    std::vector<std::string> insertLines; ///< 插入的新行 (可能含 `list:` 与空行)
    size_t                   insertAfter = lines.empty() ? 0 : lines.size() - 1;
    bool                     appendAtEnd = lines.empty();

    size_t modelLine = lines.size();
    for (size_t i = 0; i < lines.size(); ++i) {
        if (isRootKeyLine(lines[i], "model")) {
            modelLine = i;
            break;
        }
    }

    if (modelLine == lines.size()) {
        // 没有 model 段: 文件末尾追加 (与配置模板一致: list 缩进 2, 条目缩进 4)
        appendAtEnd = true;
        if (!lines.empty()) {
            insertLines.push_back("");
        }
        insertLines.push_back("model:");
        insertLines.push_back("  list:");
        auto entry = modelEntryLines(mc, 4);
        insertLines.insert(insertLines.end(), entry.begin(), entry.end());
    } else {
        if (!inlineValueOf(lines[modelLine], "model").empty()) {
            return std::unexpected{"不支持的配置结构: `model` 段是内联写法, 请手工添加模型"};
        }
        // 段范围: 到下一个根级键为止 (空行不结束段)
        size_t sectionEnd = lines.size();
        for (size_t i = modelLine + 1; i < lines.size(); ++i) {
            if (lines[i].empty()) {
                continue;
            }
            if (indentOf(lines[i]) == 0) {
                sectionEnd = i;
                break;
            }
        }

        // `list:` 行 (取缩进最小的那个)
        size_t listLine   = sectionEnd;
        int    listIndent = 0;
        for (size_t i = modelLine + 1; i < sectionEnd; ++i) {
            if (lines[i].empty()) {
                continue;
            }
            const int  ind = indentOf(lines[i]);
            const auto t   = trimmedView(lines[i]);
            if (ind <= 0 || !t.starts_with("list") || t.size() <= 4 || t[4] != ':') {
                continue;
            }
            if (listLine == sectionEnd || ind < listIndent) {
                listLine   = i;
                listIndent = ind;
            }
        }

        if (listLine != sectionEnd) {
            const std::string inlineVal = inlineValueOf(lines[listLine], "list");
            if (inlineVal == "[]") {
                // 空流式序列: 改写成块序列 (保留原有行尾注释)
                std::string rewritten
                    = std::string(static_cast<size_t>(listIndent), ' ') + "list:";
                if (const auto commentPos = lines[listLine].find('#');
                    commentPos != std::string::npos) {
                    rewritten += " ";
                    rewritten += lines[listLine].substr(commentPos);
                }
                lines[listLine] = std::move(rewritten);
                insertAfter     = listLine;
                insertLines     = modelEntryLines(mc, listIndent + 2);
            } else if (inlineVal.empty()) {
                // 块序列: 取条目缩进, 插到最后一个条目之后 (list 块之外的内容之前)
                int    itemIndent = listIndent + 2;
                bool   itemFound  = false;
                size_t lastLine   = listLine;
                for (size_t i = listLine + 1; i < sectionEnd; ++i) {
                    if (lines[i].empty() || isBlankOrComment(lines[i])) {
                        continue;
                    }
                    if (indentOf(lines[i]) <= listIndent) {
                        break;
                    }
                    if (!itemFound) {
                        if (!trimmedView(lines[i]).starts_with('-')) {
                            return std::unexpected{
                                "不支持的配置结构: model.list 下存在非条目内容, 请手工添加模型"
                            };
                        }
                        itemIndent = indentOf(lines[i]);
                        itemFound  = true;
                    }
                    lastLine = i;
                }
                insertAfter = lastLine;
                insertLines = modelEntryLines(mc, itemIndent);
            } else {
                return std::unexpected{"不支持的配置结构: model.list 是内联写法, 请手工添加模型"};
            }
        } else {
            // 段内没有 list: 追加 `list:` 行与条目 (缩进沿用段内既有子键)
            int childIndent = 2;
            for (size_t i = modelLine + 1; i < sectionEnd; ++i) {
                if (lines[i].empty() || isBlankOrComment(lines[i])) {
                    continue;
                }
                childIndent = indentOf(lines[i]);
                break;
            }
            size_t lastLine = modelLine;
            for (size_t i = modelLine + 1; i < sectionEnd; ++i) {
                if (lines[i].empty() || isBlankOrComment(lines[i])) {
                    continue;
                }
                lastLine = i;
            }
            insertAfter = lastLine;
            insertLines.push_back(std::string(static_cast<size_t>(childIndent), ' ') + "list:");
            auto entry = modelEntryLines(mc, childIndent + 2);
            insertLines.insert(insertLines.end(), entry.begin(), entry.end());
        }
    }

    // ---- 组装新内容 ----
    std::vector<std::string> out;
    if (!appendAtEnd) {
        out.assign(lines.begin(), lines.begin() + static_cast<std::ptrdiff_t>(insertAfter) + 1);
    } else {
        out = lines;
    }
    out.insert(out.end(), insertLines.begin(), insertLines.end());
    if (!appendAtEnd) {
        out.insert(out.end(), lines.begin() + static_cast<std::ptrdiff_t>(insertAfter) + 1, lines.end());
    }
    // 新建文件/末尾追加时保证以换行结尾
    const bool  newEndsWithEol = endsWithEol || appendAtEnd;
    std::string newText        = joinLines(out, eol) + (newEndsWithEol ? eol : std::string{});
    if (!hasContent) {
        newText = std::string{kNewFileHeader} + newText;
    }

    // ---- 写入前在内存里按新内容校验, 不通过就不动文件 ----
    if (auto check = verifyNewText(newText, mc); !check.has_value()) {
        return check;
    }

    // ---- 写盘 (先写临时文件再改名: 写入失败时原文件保持原样) ----
    if (auto written = writeTextAtomically(path, newText, original); !written.has_value()) {
        return written;
    }
    XX_LOGI(
        "[Config] model '{}' appended to yaml config: {}",
        mc.name,
        utilxx_base::pathToUtf8Generic(path)
    );
    return {};
}

// ---------------------------------------------------------------------------
// 删除写入
// ---------------------------------------------------------------------------

std::expected<bool, std::string>
    removeModelConfigFromYamlFile(std::string_view yamlPath, std::string_view modelName) {
    if (yamlPath.empty()) {
        return std::unexpected{"配置路径为空"};
    }
    if (trimWhitespace(modelName).empty()) {
        return std::unexpected{"模型名称不能为空"};
    }
    const auto path = utilxx_base::utf8ToPath(yamlPath);

    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        return false; // 文件不存在 = 没有需要删除的条目
    }
    auto text = readFileText(path);
    if (!text.has_value()) {
        return std::unexpected{"读取配置文件失败"};
    }
    const std::string original = std::move(*text);
    if (trimWhitespace(original).empty()) {
        return false; // 空文件 = 没有需要删除的条目
    }

    // 用行扫描定位条目 (与追加写入共用同一套结构假设, 不依赖 yaml 节点类型判定):
    // 文件里没有该条目时返回 false —— 模型可能来自 overlay 层配置或运行时注入
    std::string              eol;
    std::vector<std::string> lines = splitLines(original, eol);
    std::string              scanErr;
    auto                     entries = scanModelListEntries(lines, scanErr);
    if (!entries.has_value()) {
        return std::unexpected{fmt::format("配置文件结构不支持删除: {}", scanErr)};
    }
    size_t entryBegin = lines.size();
    size_t entryEnd   = lines.size();
    for (const auto& entry : *entries) {
        if (entry.name == modelName) {
            entryBegin = entry.begin;
            entryEnd   = entry.end;
            break;
        }
    }
    if (entryBegin == lines.size()) {
        return false;
    }
    // 条目上方紧跟的"由界面添加"说明注释随条目一起删除 (其余注释保留)
    if (entryBegin > 0 && trimmedView(lines[entryBegin - 1]) == kEntryComment) {
        --entryBegin;
    }

    std::vector<std::string> out;
    out.reserve(lines.size());
    out.insert(out.end(), lines.begin(), lines.begin() + static_cast<std::ptrdiff_t>(entryBegin));
    out.insert(out.end(), lines.begin() + static_cast<std::ptrdiff_t>(entryEnd), lines.end());
    std::string   newText = joinLines(out, eol);
    constexpr char kLast = '\n';
    if (!original.empty() && original.back() == kLast) {
        newText.push_back(kLast);
    }

    // ---- 写入前回读校验: 目标条目必须已消失, 条目数只减 1 ----
    // 同样用行扫描判定 (删掉最后一个条目后 `list:` 下面没有内容, yaml 会把它解析为
    // null 而不是空序列, 按节点类型判定会把这种合法结果误判为"不是列表")
    {
        std::string verifyErr;
        std::string verifyEol;
        auto        after
            = scanModelListEntries(splitLines(newText, verifyEol), verifyErr);
        if (!after.has_value()) {
            return std::unexpected{fmt::format("删除后校验失败: {}", verifyErr)};
        }
        for (const auto& entry : *after) {
            if (entry.name == modelName) {
                return std::unexpected{"删除后校验失败: 目标模型仍然存在"};
            }
        }
        if (after->size() + 1 != entries->size()) {
            return std::unexpected{"删除后校验失败: 条目数量变化异常"};
        }
    }

    if (auto written = writeTextAtomically(path, newText, original); !written.has_value()) {
        return std::unexpected{written.error()};
    }
    XX_LOGI(
        "[Config] model '{}' removed from yaml config: {}",
        modelName,
        utilxx_base::pathToUtf8Generic(path)
    );
    return true;
}

} // namespace agent
} // namespace agentxx
