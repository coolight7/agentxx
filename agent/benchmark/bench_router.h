#pragma once

#include "agentxx/util/router.h"
#include "bench_util.h"
#include <memory>
#include <string>

namespace agentxx {
namespace bench {

struct DummyHandle {
  std::string name;
  explicit DummyHandle(std::string_view n) : name(n) {}
};

inline void benchRouter() {
  std::cout << "\n=== XXRouter Benchmarks ===" << std::endl;

  constexpr size_t HANDLE_NUM = 4;

  {
    XXRouter<DummyHandle, HANDLE_NUM> router;
    for (int i = 0; i < 1000; ++i) {
      auto handle =
          std::make_shared<DummyHandle>("handler_" + std::to_string(i));
      router.add("/api/v1/resource" + std::to_string(i), 0, handle);
    }

    auto r =
        runBench("XXRouter::get [1000 routes, cached lookup]", 100000, [&]() {
          std::string re_path;
          auto result = router.get("/api/v1/resource500", 0, re_path);
          (void)result;
        });
    printResult(r);
  }

  {
    XXRouter<DummyHandle, HANDLE_NUM> router;
    for (int i = 0; i < 1000; ++i) {
      auto handle =
          std::make_shared<DummyHandle>("handler_" + std::to_string(i));
      router.add("/api/v1/resource" + std::to_string(i), 0, handle);
    }

    auto r =
        runBench("XXRouter::getNocache [1000 routes, no cache]", 100000, [&]() {
          std::string re_path;
          auto result = router.getNocache("/api/v1/resource500", 0, re_path);
          (void)result;
        });
    printResult(r);
  }

  {
    XXRouter<DummyHandle, HANDLE_NUM> router;
    for (int i = 0; i < 1000; ++i) {
      auto handle =
          std::make_shared<DummyHandle>("handler_" + std::to_string(i));
      router.add("/api/v1/resource" + std::to_string(i), 0, handle);
    }

    auto r = runBench("XXRouter::add [1000 routes]", 10, [&]() {
      XXRouter<DummyHandle, HANDLE_NUM> localRouter;
      for (int i = 0; i < 1000; ++i) {
        auto handle =
            std::make_shared<DummyHandle>("handler_" + std::to_string(i));
        localRouter.add("/api/v1/resource" + std::to_string(i), 0, handle);
      }
    });
    printResult(r);
  }

  {
    XXRouter<DummyHandle, HANDLE_NUM> router;
    for (int i = 0; i < 100; ++i) {
      for (int j = 0; j < 10; ++j) {
        auto handle = std::make_shared<DummyHandle>(
            "handler_" + std::to_string(i) + "_" + std::to_string(j));
        router.add("/api/v" + std::to_string(i) + "/resource" +
                       std::to_string(j),
                   0, handle);
      }
    }

    auto r = runBench(
        "XXRouter::get [1000 routes, deep paths, cached]", 100000, [&]() {
          std::string re_path;
          auto result = router.get("/api/v50/resource5", 0, re_path);
          (void)result;
        });
    printResult(r);
  }

  {
    XXRouter<DummyHandle, HANDLE_NUM> router;
    auto handle = std::make_shared<DummyHandle>("wildcard_handler");
    router.add("/api/*", 0, handle);

    auto r = runBench("XXRouter::get [wildcard route, cached]", 500000, [&]() {
      std::string re_path;
      auto result = router.get("/api/anything/here", 0, re_path);
      (void)result;
    });
    printResult(r);
  }

  {
    XXRouter<DummyHandle, HANDLE_NUM> router;
    for (int i = 0; i < 100; ++i) {
      auto handle =
          std::make_shared<DummyHandle>("handler_" + std::to_string(i));
      router.add("/short/" + std::to_string(i), 0, handle);
    }

    auto r =
        runBench("XXRouter::get [100 short routes, cached]", 500000, [&]() {
          std::string re_path;
          auto result = router.get("/short/50", 0, re_path);
          (void)result;
        });
    printResult(r);
  }

  {
    XXRouter<DummyHandle, HANDLE_NUM> router;
    for (int i = 0; i < 10000; ++i) {
      auto handle =
          std::make_shared<DummyHandle>("handler_" + std::to_string(i));
      router.add("/api/v1/resource" + std::to_string(i), 0, handle);
    }

    auto r =
        runBench("XXRouter::get [10000 routes, cached lookup]", 50000, [&]() {
          std::string re_path;
          auto result = router.get("/api/v1/resource5000", 0, re_path);
          (void)result;
        });
    printResult(r);
  }

  {
    XXRouter<DummyHandle, HANDLE_NUM> router;
    for (int i = 0; i < 10000; ++i) {
      auto handle =
          std::make_shared<DummyHandle>("handler_" + std::to_string(i));
      router.add("/api/v1/resource" + std::to_string(i), 0, handle);
    }

    auto r =
        runBench("XXRouter::getNocache [10000 routes, no cache]", 50000, [&]() {
          std::string re_path;
          auto result = router.getNocache("/api/v1/resource5000", 0, re_path);
          (void)result;
        });
    printResult(r);
  }
}

} // namespace bench
} // namespace agentxx