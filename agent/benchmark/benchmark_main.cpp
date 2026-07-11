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

#if XX_IS_DEBUG_D && XX_IS_LINUX_D
#include "agentxx/util/log.h"
#endif

int main(int argn, char **argv) {
#if XX_IS_DEBUG_D && XX_IS_LINUX_D
  agentxx::util::signalError(argv[0]);
#endif

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
    config.iterations = 5;

    agentxx::bench::benchDeepAgentInit();
    agentxx::bench::benchDeepAgentRunConversationTurnAsync(config);
  }

  std::cout << "\n========================================" << std::endl;
  std::cout << "  All Benchmarks Complete" << std::endl;
  std::cout << "========================================" << std::endl;

  return 0;
}