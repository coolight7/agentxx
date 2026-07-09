#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace agentxx {
namespace expand {

enum class AudioDataSource {
  SystemOutput,
  ProgramOutput,
  MicrophoneInput,
};

struct AudioData {
  std::vector<uint8_t> data;
  uint32_t sampleRate = 0;
  uint16_t channels = 0;
  uint16_t bitsPerSample = 0;
  AudioDataSource source = AudioDataSource::SystemOutput;
  std::chrono::steady_clock::time_point timestamp;
  uint32_t processId = 0;
  std::string processName;
};

using AudioStreamListener = std::function<void(const AudioData &data)>;

class AudioStream {
public:
  AudioStream();
  ~AudioStream();

  AudioStream(const AudioStream &) = delete;
  AudioStream &operator=(const AudioStream &) = delete;

  bool start(AudioDataSource source, uint32_t targetProcessId = 0);
  void stop();
  bool isRunning() const;

  void addListener(AudioStreamListener listener);
  void removeAllListeners();

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace expand
} // namespace agentxx