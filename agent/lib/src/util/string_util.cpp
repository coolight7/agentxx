#include "agentxx/util/string_util.h"
#include "uchardet/uchardet.h"
#include <algorithm>
#include <cstring>
#include <iconv.h>
#include <map>
#include <mutex>
#include <set>
#include <vector>

const agentxx::util::IgnoreCaseSet g_encoding_priorities = {
    "UTF-8", "UTF-16", "GB2312", "GBK", "GB18030",
};
const agentxx::util::IgnoreCaseMap<std::string> g_encoding_shift = {
    {"UTF8", "UTF-8"},
    {"UTF16", "UTF-16"},
    {"GB2312", "GB18030"},
    {"GBK", "GB18030"},
};
const size_t defShortStringLength = 30;

static const thread_local uchardet_t g_chardetHandle = uchardet_new();

// BOM 判断UTF16编码
static std::string detectUtfBom(std::string_view str) {
  if (str.size() < 2) {
    // UTF16 BOM至少2字节，长度不足直接返回
    return "";
  }
  const unsigned char *bom =
      reinterpret_cast<const unsigned char *>(str.data());
  if (bom[0] == 0xFF && bom[1] == 0xFE) {
    // UTF-16LE BOM (0xFF 0xFE)
    return "UTF-16LE";
  } else if (bom[0] == 0xFE && bom[1] == 0xFF) {
    // UTF-16BE BOM (0xFE 0xFF)
    return "UTF-16BE";
  } else if (str.size() >= 3 && bom[0] == 0xEF && bom[1] == 0xBB &&
             bom[2] == 0xBF) {
    return "UTF-8";
  }
  // 无BOM，返回空
  return "";
}

static std::vector<std::string>
getIconvCandidateEncodings(const char *src_encoding) {
  std::vector<std::string> candidates;
  if (src_encoding == nullptr) {
    return candidates;
  }
  std::string enc = src_encoding;
  // UTF-16LE，Windows默认
  if (enc == "UTF-16LE" || enc == "UTF16LE" || enc == "UTF-16") {
    candidates = {"UTF-16LE", "UCS-2LE", "UTF16LE"};
  } else if (enc == "UTF-16BE" || enc == "UTF16BE") {
    // UTF-16BE
    candidates = {"UTF-16BE", "UCS-2BE", "UTF16BE"};
  } else {
    // 其他编码
    candidates.push_back(enc);
  }
  return candidates;
}

std::tuple<bool, std::optional<std::string>>
agentxx::util::convertCharset(std::string_view src,
                              std::string_view srcEncoding,
                              std::string_view targetEncoding) {
  // 已是目标编码/空字符串，直接返回
  if (agentxx::util::isIgnoreCaseEqual(srcEncoding, targetEncoding)) {
    return {true, std::nullopt};
  }
  if (src.empty()) {
    return {false, std::nullopt};
  }

  auto candidate_encs = getIconvCandidateEncodings(srcEncoding.data());
  if (candidate_encs.empty()) {
    return {false, std::nullopt};
  }

  // 遍历候选编码，逐个尝试
  auto target_enc = fmt::format("{}//TRANSLIT//IGNORE",
                                agentxx::util::toUpper(targetEncoding));
  iconv_t cd = (iconv_t)-1;
  // 记录实际使用的成功编码名
  std::string used_src_enc;
  for (const auto &enc : candidate_encs) {
    std::string src_enc = enc + "//TRANSLIT//IGNORE";
    cd = iconv_open(target_enc.c_str(), src_enc.c_str());
    if (cd != (iconv_t)-1) {
      used_src_enc = enc;
      break;
    }
  }
  if (cd == (iconv_t)-1) {
    // 所有候选编码名都失败，直接返回
    return {false, std::nullopt};
  }

  // 缓冲区
  size_t src_len = src.size();
  // 最大预期，单字节转换为 8字节编码长度
  size_t dst_buf_size = src_len * 8;
  std::vector<char> dst_buf(dst_buf_size);
  char *dst_ptr = dst_buf.data();
  size_t dst_remain = dst_buf_size;

  // 编码转换
  const char *src_ptr = src.data();
  size_t src_remain = src_len;
  auto ret = iconv(cd, const_cast<char **>(&src_ptr), &src_remain, &dst_ptr,
                   &dst_remain);

  std::string targetStr;
  if (ret != static_cast<size_t>(-1) && src_remain == 0) {
    targetStr = std::string(dst_buf.data(), dst_buf_size - dst_remain);
  }

  iconv_close(cd);
  return {true, targetStr};
}

/// <isSuccess, result>
std::tuple<bool, std::optional<std::string>>
agentxx::util::autoConvertCharset(std::string_view str, std::string &encoding,
                                  std::string_view targetEncoding) {
  if (str.empty()) {
    return {true, std::nullopt};
  }

  // 手动检测UTF16 BOM
  std::string bom_enc = detectUtfBom(str);
  if (!bom_enc.empty()) {
    encoding = bom_enc;
    auto [isSuccess, result] =
        agentxx::util::convertCharset(str, encoding, targetEncoding);
    if (isSuccess) {
      return {true, result};
    }
  }

  // uchardet检测
  uchardet_reset(g_chardetHandle);
  int ret = uchardet_handle_data(g_chardetHandle, str.data(), str.size());
  if (ret != 0) {
    return {false, std::nullopt};
  }
  uchardet_data_end(g_chardetHandle);

  auto n_candidates = uchardet_get_n_candidates(g_chardetHandle);
  if (n_candidates <= 0) {
    return {false, std::nullopt};
  }
  // if (n_candidates > 5) {
  //     n_candidates = 5;
  // }
  std::vector<std::string> detected_candidates;
  for (size_t i = 0; i < n_candidates; ++i) {
    const char *enc = uchardet_get_encoding(g_chardetHandle, i);
    if (enc && std::strlen(enc) > 0) {
      detected_candidates.emplace_back(enc);
    }
  }
  if (detected_candidates.empty()) {
    return {false, std::nullopt};
  }

  std::string selected_enc;
  {
    bool haveCheckUtf8 = false;
    // detected_candidates 中可能出现多次 utf8
    for (const auto &item : detected_candidates) {
      if (g_encoding_priorities.contains(item)) {
        auto item_ptr = &item;

        auto shiftItemIt = g_encoding_shift.find(item);
        if (shiftItemIt != g_encoding_shift.end()) {
          item_ptr = &(shiftItemIt->second);
        }

        if (str.size() < defShortStringLength) {
          // 短字符串容易不准确，需要检查utf8有效性
          if (*item_ptr == std::string_view{"UTF-8"}) {
            if (haveCheckUtf8 ||
                false == agentxx::util::utf8GetLengthCheckAvail(str.data())) {
              haveCheckUtf8 = true;
              continue;
            }
          }
        }
        selected_enc = *item_ptr;
        break;
      }
    }
  }
  if (selected_enc.empty()) {
    // 无优先，取第一个
    if (str.size() < defShortStringLength) {
      // 短字符串直接取 gb
      selected_enc = "GB18030";
    } else {
      selected_enc = detected_candidates[0];
    }
  }

  encoding = selected_enc;
  if (encoding == "UTF-16") {
    // 通用UTF-16, 强制转为UTF-16LE，适配Windows
    encoding = "UTF-16LE";
  }

  return agentxx::util::convertCharset(str, encoding, targetEncoding);
}

std::tuple<bool, std::optional<std::string>>
agentxx::util::autoConvertToUtf8(std::string_view str, bool _) {
  std::string encoding;
  return autoConvertCharset(str, encoding, "UTF-8");
}

bool agentxx::util::autoConvertToUtf8(std::string &str) {
  auto [isSuccess, result] = autoConvertToUtf8(str, true);
  if (isSuccess && result.has_value()) {
    str = std::move(result.value());
  }
  return isSuccess;
}

std::string agentxx::util::autoTryConvertToUtf8(std::string_view str) {
  auto [isSuccess, result] = autoConvertToUtf8(str, true);
  if (isSuccess && result.has_value()) {
    return std::move(result.value());
  }
  return std::string{str};
}

bool agentxx::util::autoConvertToSystemPath(std::string &str) {
  // TODO: 适配windows转换字符编码
  return true;
#if XX_IS_WIN_D
  std::string encoding;
  auto [isSuccess, result] = autoConvertCharset(str, encoding, "GBK");
  if (isSuccess && result.has_value()) {
    str = std::move(result.value());
  }
  return isSuccess;
#else
  return autoConvertToUtf8(str);
#endif
}
