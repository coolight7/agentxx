#pragma once

#include "agentxx/middlewares/middleware.h"
#include "agentxx/events.h"
#include "agentxx/middlewares/event_stream.h"
#include "agentxx/util/router.h"
#include "asio/io_context.hpp"
#include "fmt/format.h"
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <neograph/neograph.h>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace agentxx {
namespace middleware {

enum class PermissionOperator {
  /// 允许
  ALLOW,

  /// 拒绝
  DENY,

  /// 中断,询问用户是否同意
  INTERRUPT,
};

class PermissionMiddlewareState : public BaseMiddlewareState {
public:
  PermissionMiddlewareState() {}
};

class PermissionMiddlewareHandle
    : public BaseMiddlewareHandle<PermissionMiddlewareState> {
protected:
public:
  inline static constexpr size_t FilesystemPermissionREAD = 0;
  inline static constexpr size_t FilesystemPermissionWRITE = 1;

  /// 遵循最长路径匹配，支持 * 通配符
  XXRouter<PermissionOperator, 2> filesystemPermission{};
  /// <name, handle>
  std::map<std::string, std::function<asio::awaitable<bool>(
                            const neograph::Tool &item, neograph::json &args)>>
      handles{};

  PermissionMiddlewareHandle(
      std::weak_ptr<agentxx::agent::AgentContext> in_agentContext)
      : BaseMiddlewareHandle<PermissionMiddlewareState>(
            "PermissionMiddlewareHandle", in_agentContext) {}

  void setFilesystemPermission(std::string_view path, PermissionOperator op,
                               size_t index) {
    assert(index == 0 || index == 1);
    filesystemPermission.add(path, index,
                             std::make_shared<PermissionOperator>(op));
  }

  asio::awaitable<bool> defOnFilesystemHandle(const neograph::Tool &item,
                                              neograph::json &args,
                                              size_t index) {
    auto path = args.value<std::string>("path", "");
    std::string re_path;
    auto handle = filesystemPermission.get(path, index, re_path);
    if (nullptr != handle) {
      auto permission = *handle;
      switch (permission) {
      case PermissionOperator::ALLOW:
        co_return true;
      case PermissionOperator::DENY:
        co_return false;
      case PermissionOperator::INTERRUPT:
        // 经总线询问外部授权者 (CLI/GUI/ACP 各注册自己的 prompter)
        // - 无 prompter 注册时 request 返回 nullopt, 默认拒绝以保安全
        co_return co_await requestPermission(
            item, args,
            index == FilesystemPermissionREAD ? "filesystem_read"
                                              : "filesystem_write",
            path);
      }
    }
    co_return true;
  }

  /// 经总线发起权限询问; 无 prompter 或被拒绝时返回 false
  asio::awaitable<bool> requestPermission(const neograph::Tool &item,
                                          neograph::json &args,
                                          std::string category,
                                          std::string target) {
    auto ctxPtr = agentContext.lock();
    if (!ctxPtr || !ctxPtr->bus) {
      // 无总线, 默认拒绝以保安全
      co_return false;
    }
    auto resp = co_await ctxPtr->bus->request<events::ReqPermission,
                                              events::RespPermission>(
        events::Topic::Permission,
        events::ReqPermission{
            .agentName = ctxPtr->agentConfig ? ctxPtr->agentConfig->agentName
                                             : std::string{},
            .threadId = args.value("thread_id", std::string{}),
            .toolName = item.get_name(),
            .category = std::move(category),
            .target = std::move(target),
            .argumentsJson = args.dump(),
        });
    if (!resp.has_value()) {
      co_return false; // 无 prompter, 拒绝
    }
    co_return resp->decision == events::RespPermission::Decision::Allow;
  }

  void registerFilesystemHandles() {
    auto readHandle = [this](const neograph::Tool &item,
                             neograph::json &args) -> asio::awaitable<bool> {
      co_return co_await defOnFilesystemHandle(item, args,
                                               FilesystemPermissionREAD);
    };

    handles["filesystem_list"] = readHandle;
    handles["filesystem_read_text_file"] = readHandle;
    handles["filesystem_read_binary_file"] = readHandle;
    handles["filesystem_write_file"] =
        [this](const neograph::Tool &item,
               neograph::json &args) -> asio::awaitable<bool> {
      co_return co_await defOnFilesystemHandle(item, args,
                                               FilesystemPermissionWRITE);
    };
    handles["filesystem_edit_text_file"] =
        [this](const neograph::Tool &item,
               neograph::json &args) -> asio::awaitable<bool> {
      co_return co_await defOnFilesystemHandle(item, args,
                                               FilesystemPermissionREAD) &&
          co_await defOnFilesystemHandle(item, args, FilesystemPermissionWRITE);
    };
    // handles["filesystem_glob"] = readHandle;
    // handles["filesystem_grep"] = readHandle;
  }

  void registerHandles() { registerFilesystemHandles(); }
};

} // namespace middleware
} // namespace agentxx