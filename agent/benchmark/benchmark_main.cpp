#include "agentxx/util/log.h"
#include "bench_aho_corasick.h"
#include "bench_deepagent.h"
#include "bench_regex.h"
#include "bench_router.h"
#include "bench_string_util.h"
#include "bench_util.h"
#include <cstdlib>
#include <iostream>
#include <string>

#ifndef AGENTXX_BENCH_OUTPUT_DIR
#define AGENTXX_BENCH_OUTPUT_DIR ""
#endif

#if XX_IS_DEBUG_D && XX_IS_LINUX_D
#include "agentxx/util/log.h"
#endif

int main(int argn, char **argv) {
#if XX_IS_DEBUG_D && XX_IS_LINUX_D
  agentxx::util::signalError(argv[0]);
#endif

  auto &reporter = agentxx::bench::BenchReporter::instance();
  std::string outputDir = AGENTXX_BENCH_OUTPUT_DIR;
  if (const char *envDir = std::getenv("AGENTXX_BENCH_OUTPUT_DIR")) {
    outputDir = envDir;
  }
  if (!outputDir.empty()) {
    reporter.setOutputDir(outputDir);
    std::cout << "[BenchReporter] output dir: " << outputDir << std::endl;
  }

  std::cout << "========================================" << std::endl;
  std::cout << "  agentxx Performance Benchmarks" << std::endl;
  std::cout << "========================================" << std::endl;

  agentxx::bench::benchStringUtil();
  agentxx::bench::benchAhoCorasick();
  agentxx::bench::benchRegex();
  agentxx::bench::benchRouter();

  {
    agentxx::bench::DeepAgentBenchConfig config;
    config.openAIBaseUrl = std::getenv("AGENTXX_BENCH_LLM_BASE_URL")
                               ? std::getenv("AGENTXX_BENCH_LLM_BASE_URL")
                               : "";
    config.openAIApiKey = std::getenv("AGENTXX_BENCH_LLM_API_KEY")
                              ? std::getenv("AGENTXX_BENCH_LLM_API_KEY")
                              : "EMPTY";
    config.openAIModelName = std::getenv("AGENTXX_BENCH_LLM_MODEL_NAME")
                                 ? std::getenv("AGENTXX_BENCH_LLM_MODEL_NAME")
                                 : "Agentxx";
    config.systemPrompt = std::getenv("AGENTXX_BENCH_LLM_SYSTEM_PROMPT")
                              ? std::getenv("AGENTXX_BENCH_LLM_SYSTEM_PROMPT")
                              : "You are a helpful assistant.";
    config.userInput = std::getenv("AGENTXX_BENCH_LLM_USER_INPUT")
                           ? std::getenv("AGENTXX_BENCH_LLM_USER_INPUT")
                           : "Hello, please respond briefly.";
    // If no external LLM URL is configured, the individual benchmarks will
    // auto-start a local sim server.
    config.iterations = 5;

    agentxx::bench::benchDeepAgentInit();
    agentxx::bench::benchDeepAgentInitWarm();
    agentxx::bench::benchDeepAgentRunConversationTurnAsync(config);
    agentxx::bench::benchDeepAgentSimpleCompletion(config);
    agentxx::bench::benchDeepAgentMultiTurn(config);
    agentxx::bench::benchDeepAgentLargeHistory(config);
  }

  reporter.flushToFile();

  std::cout << "\n========================================" << std::endl;
  std::cout << "  All Benchmarks Complete" << std::endl;
  std::cout << "========================================" << std::endl;

  return 0;
}