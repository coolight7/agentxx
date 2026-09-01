#include "agentxx/util/env.h"

#include <cstdlib>
#include <string>
#include <string_view>

namespace agentxx {
namespace util {

ApplicationEnv& ApplicationEnv::instance() {
    static ApplicationEnv s_instance;
    return s_instance;
}

void ApplicationEnv::set(std::string_view name, std::string_view value) {
    if (name.empty()) {
        return;
    }
    std::lock_guard<std::mutex> lk(m_mutex);
    auto                        it = m_preset.find(name);
    if (it != m_preset.end()) {
        it->second.assign(value.data(), value.size());
    } else {
        m_preset.emplace(std::string{name}, std::string{value});
    }
}

void ApplicationEnv::set(std::string_view name, std::string&& value) {
    if (name.empty()) {
        return;
    }
    std::lock_guard<std::mutex> lk(m_mutex);
    auto                        it = m_preset.find(name);
    if (it != m_preset.end()) {
        it->second = std::move(value);
    } else {
        m_preset.emplace(std::string{name}, std::move(value));
    }
}

void ApplicationEnv::set(std::string_view name, const std::string& value) {
    set(name, std::string_view{value});
}

void ApplicationEnv::remove(std::string_view name) {
    if (name.empty()) {
        return;
    }
    std::lock_guard<std::mutex> lk(m_mutex);
    auto                        it = m_preset.find(name);
    if (it != m_preset.end()) {
        m_preset.erase(it);
    }
}

void ApplicationEnv::clear() {
    std::lock_guard<std::mutex> lk(m_mutex);
    m_preset.clear();
}

std::optional<std::string> ApplicationEnv::getPreset(std::string_view name) {
    if (name.empty()) {
        return std::nullopt;
    }
    std::lock_guard<std::mutex> lk(m_mutex);
    auto                        it = m_preset.find(name);
    if (it != m_preset.end()) {
        return it->second;
    }
    return std::nullopt;
}

bool ApplicationEnv::hasPreset(std::string_view name) {
    if (name.empty()) {
        return false;
    }
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_preset.find(name) != m_preset.end();
}

std::optional<std::string> ApplicationEnv::getSystem(std::string_view name) {
    if (name.empty()) {
        return std::nullopt;
    }
    std::string key{name};
#if XX_IS_WIN_D
    char*  buf = nullptr;
    size_t len = 0;
    // _dupenv_s 为 MSVC 推荐的安全替代 (消除 C4996), 返回的 buf 需 free
    errno_t err = _dupenv_s(&buf, &len, key.c_str());
    if (err != 0 || buf == nullptr) {
        return std::nullopt;
    }
    std::string val(buf);
    free(buf);
    return val;
#else
    const char* v = std::getenv(key.c_str());
    if (v == nullptr) {
        return std::nullopt;
    }
    return std::string{v};
#endif
}

bool ApplicationEnv::hasSystem(std::string_view name) {
    return getSystem(name).has_value();
}

std::optional<std::string> ApplicationEnv::get(std::string_view name) {
    if (auto preset = getPreset(name)) {
        return preset;
    }
    return getSystem(name);
}

std::string ApplicationEnv::getOr(std::string_view name, std::string_view defaultValue) {
    if (auto v = get(name)) {
        return *v;
    }
    return std::string{defaultValue};
}

bool ApplicationEnv::has(std::string_view name) {
    if (hasPreset(name)) {
        return true;
    }
    return hasSystem(name);
}

} // namespace util
} // namespace agentxx
