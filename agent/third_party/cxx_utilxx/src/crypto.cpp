#include "utilxx/crypto.h"

#include "utilxx_base/hash.h"
#include "utilxx_base/log.h"
#include "utilxx_base/string_util.h"
#include "fmt/format.h"
#include <cstdint>
#include <fstream>
#include <mutex>
#include <openssl/evp.h>
#include <string>
#include <string_view>

// 平台 API: Windows 注册表 MachineGuid / POSIX 主机名
#if XX_IS_WIN_D
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace utilxx {
namespace {

#if XX_IS_WIN_D
/// Windows 平台下读取机器特征: 优先尝试注册表 MachineGuid, 失败则读取计算机名
std::string readMachineGuidWin() {
    HKEY hKey = nullptr;
    if (RegOpenKeyExA(
            HKEY_LOCAL_MACHINE,
            "SOFTWARE\\Microsoft\\Cryptography",
            0,
            KEY_READ | KEY_WOW64_64KEY,
            &hKey
        )
        == ERROR_SUCCESS) {
        char  buf[256] = {0};
        DWORD bufSize  = sizeof(buf);
        DWORD type     = 0;
        if (RegQueryValueExA(
                hKey,
                "MachineGuid",
                nullptr,
                &type,
                reinterpret_cast<LPBYTE>(buf),
                &bufSize
            )
            == ERROR_SUCCESS) {
            RegCloseKey(hKey);
            return std::string(buf);
        }
        RegCloseKey(hKey);
    }
    char  compName[MAX_COMPUTERNAME_LENGTH + 1] = {0};
    DWORD compSize                              = sizeof(compName);
    if (GetComputerNameA(compName, &compSize)) {
        return std::string(compName);
    }
    return {};
}
#elif XX_IS_LINUX_D || XX_IS_ANDROID_D
/// Linux/Android 平台下读取机器特征: 优先读取 machine-id, 失败则读取主机名
std::string readMachineIdLinux() {
    for (const char* path : {"/etc/machine-id", "/var/lib/dbus/machine-id"}) {
        std::ifstream ifs(path);
        if (ifs.is_open()) {
            std::string id;
            if (std::getline(ifs, id)) {
                id = utilxx_base::removeBetweenSpace(id);
                if (!id.empty()) {
                    return id;
                }
            }
        }
    }
    char hostname[256] = {0};
    if (gethostname(hostname, sizeof(hostname)) == 0 && hostname[0] != '\0') {
        return std::string(hostname);
    }
    return {};
}
#endif

/// 跨平台获取当前设备特征的原始字符串
std::string readPlatformRawDeviceId() {
#if XX_IS_WIN_D
    auto id = readMachineGuidWin();
    if (!id.empty()) {
        return id;
    }
#elif XX_IS_LINUX_D || XX_IS_ANDROID_D
    auto id = readMachineIdLinux();
    if (!id.empty()) {
        return id;
    }
#else
    char hostname[256] = {0};
    if (gethostname(hostname, sizeof(hostname)) == 0 && hostname[0] != '\0') {
        return std::string(hostname);
    }
#endif
    return "agentxx_default_device";
}

} // namespace

std::string md5Hex(std::string_view input) {
    unsigned char digest[16] = {0};
    unsigned int  digestLen  = 0;
    EVP_MD_CTX*   ctx        = EVP_MD_CTX_new();
    if (ctx) {
        if (EVP_DigestInit_ex(ctx, EVP_md5(), nullptr) == 1) {
            EVP_DigestUpdate(ctx, input.data(), input.size());
            // 取摘要失败时保持 digestLen == 0, 交由下方兜底
            if (EVP_DigestFinal_ex(ctx, digest, &digestLen) != 1) {
                digestLen = 0;
            }
        }
        EVP_MD_CTX_free(ctx);
    }
    if (digestLen == 0) {
        // OpenSSL 计算失败 (罕见): 记录日志并回退 FNV-1a 哈希 ——
        // 调用方据此生成设备 id (WireHelloAck.deviceId), 静默返回空串会让
        // 客户端无法区分设备且难以排查
        XX_LOGE("md5Hex failed (OpenSSL digest error), falling back to FNV-1a hash");
        uint64_t h = utilxx_base::hash::kFnv1a64OffsetBasis;
        for (char c : input) {
            h ^= static_cast<unsigned char>(c);
            h *= utilxx_base::hash::kFnv1a64Prime;
        }
        // 输出 32 位十六进制 (与 MD5 输出形态一致, 调用方无需按长度分支处理)
        return fmt::format("{:016x}{:016x}", h, h ^ 0x9E3779B97F4A7C15ULL);
    }
    std::string hex;
    hex.reserve(32);
    for (unsigned int i = 0; i < digestLen; ++i) {
        hex += fmt::format("{:02x}", digest[i]);
    }
    return hex;
}

std::string getDeviceId() {
    static std::string    cachedId;
    static std::once_flag flag;
    std::call_once(flag, []() { cachedId = md5Hex(readPlatformRawDeviceId()); });
    return cachedId;
}

} // namespace utilxx
