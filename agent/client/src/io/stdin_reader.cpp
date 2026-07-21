#include "agentxx-client/io/stdin_reader.h"

StdinReader &StdinReader::instance(asio::any_io_executor ex) {
  static std::shared_ptr<StdinReader> inst;
  static std::once_flag flag;
  std::call_once(flag, [&]() {
    inst = std::shared_ptr<StdinReader>(new StdinReader{ex});
  });
  return *inst;
}
