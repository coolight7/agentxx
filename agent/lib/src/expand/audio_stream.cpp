#include "agentxx/expand/audio_stream.h"
#include "agentxx/util/log.h"
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

#if XX_IS_WIN_D
#define INITGUID
#include <audioclient.h>
#include <audiopolicy.h>
#include <audiosessiontypes.h>
#include <mmdeviceapi.h>
#include <psapi.h>
#include <windows.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

#pragma comment(lib, "mmdevapi.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "psapi.lib")
#endif

namespace agentxx {
namespace expand {

#if XX_IS_WIN_D && false

class AudioStream::Impl {
public:
  Impl()
      : running_(false), source_(AudioDataSource::SystemOutput),
        targetProcessId_(0) {}

  ~Impl() { stop(); }

  bool start(AudioDataSource source, uint32_t targetProcessId) {
    if (running_.load()) {
      XX_LOGW("AudioStream: already running");
      return false;
    }

    source_ = source;
    targetProcessId_ = targetProcessId;

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
      XX_LOGE("AudioStream: CoInitializeEx failed, hr=0x{:08X}",
              static_cast<unsigned>(hr));
      return false;
    }
    comInitialized_ = SUCCEEDED(hr);

    hr = activateDevice();
    if (FAILED(hr)) {
      XX_LOGE("AudioStream: activateDevice failed, hr=0x{:08X}",
              static_cast<unsigned>(hr));
      cleanupCom();
      return false;
    }

    hr = initializeClient();
    if (FAILED(hr)) {
      XX_LOGE("AudioStream: initializeClient failed, hr=0x{:08X}",
              static_cast<unsigned>(hr));
      cleanupResources();
      return false;
    }

    running_.store(true);
    captureThread_ = std::thread(&Impl::captureLoop, this);

    const char *sourceName = "Unknown";
    switch (source_) {
    case AudioDataSource::SystemOutput:
      sourceName = "SystemOutput";
      break;
    case AudioDataSource::ProgramOutput:
      sourceName = "ProgramOutput";
      break;
    case AudioDataSource::MicrophoneInput:
      sourceName = "MicrophoneInput";
      break;
    }
    XX_LOGI("AudioStream: started (source={}, pid={})", sourceName,
            targetProcessId);
    return true;
  }

  void stop() {
    if (!running_.load()) {
      return;
    }

    running_.store(false);

    if (captureThread_.joinable()) {
      captureThread_.join();
    }

    cleanupResources();
    XX_LOGI("AudioStream: stopped");
  }

  bool isRunning() const { return running_.load(); }

  void addListener(AudioStreamListener listener) {
    std::lock_guard<std::mutex> lock(listenersMutex_);
    listeners_.push_back(std::move(listener));
  }

  void removeAllListeners() {
    std::lock_guard<std::mutex> lock(listenersMutex_);
    listeners_.clear();
  }

private:
  HRESULT activateDevice() {
    HRESULT hr = CoCreateInstance(
        CLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL, IID_IMMDeviceEnumerator,
        reinterpret_cast<void **>(enumerator_.GetAddressOf()));
    if (FAILED(hr)) {
      XX_LOGE("AudioStream: CoCreateInstance MMDeviceEnumerator failed, "
              "hr=0x{:08X}",
              static_cast<unsigned>(hr));
      return hr;
    }

    EDataFlow dataFlow;
    DWORD stateMask;

    if (source_ == AudioDataSource::MicrophoneInput) {
      dataFlow = eCapture;
      stateMask = DEVICE_STATE_ACTIVE;
    } else {
      dataFlow = eRender;
      stateMask = DEVICE_STATE_ACTIVE;
    }

    hr = enumerator_->GetDefaultAudioEndpoint(dataFlow, eConsole,
                                              device_.GetAddressOf());
    if (FAILED(hr)) {
      XX_LOGE("AudioStream: GetDefaultAudioEndpoint failed, hr=0x{:08X}",
              static_cast<unsigned>(hr));
      return hr;
    }

    hr = device_->Activate(
        IID_IAudioClient, CLSCTX_ALL, nullptr,
        reinterpret_cast<void **>(audioClient_.GetAddressOf()));
    if (FAILED(hr)) {
      XX_LOGE("AudioStream: Activate IAudioClient failed, hr=0x{:08X}",
              static_cast<unsigned>(hr));
      return hr;
    }

    return S_OK;
  }

  HRESULT initializeClient() {
    WAVEFORMATEX *mixFormat = nullptr;
    HRESULT hr = audioClient_->GetMixFormat(&mixFormat);
    if (FAILED(hr)) {
      XX_LOGE("AudioStream: GetMixFormat failed, hr=0x{:08X}",
              static_cast<unsigned>(hr));
      return hr;
    }

    sampleRate_ = mixFormat->nSamplesPerSec;
    channels_ = mixFormat->nChannels;
    bitsPerSample_ = mixFormat->wBitsPerSample;

    REFERENCE_TIME bufferDuration = 10000000;

    DWORD streamFlags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
    if (source_ == AudioDataSource::SystemOutput ||
        source_ == AudioDataSource::ProgramOutput) {
      streamFlags |= AUDCLNT_STREAMFLAGS_LOOPBACK;
    }

    hr = audioClient_->Initialize(AUDCLNT_SHAREMODE_SHARED, streamFlags,
                                  bufferDuration, 0, mixFormat, nullptr);
    if (FAILED(hr)) {
      XX_LOGE("AudioStream: Initialize failed, hr=0x{:08X}",
              static_cast<unsigned>(hr));
      CoTaskMemFree(mixFormat);
      return hr;
    }

    hr = audioClient_->GetService(
        IID_IAudioCaptureClient,
        reinterpret_cast<void **>(captureClient_.GetAddressOf()));
    if (FAILED(hr)) {
      XX_LOGE("AudioStream: GetService IAudioCaptureClient failed, hr=0x{:08X}",
              static_cast<unsigned>(hr));
      CoTaskMemFree(mixFormat);
      return hr;
    }

    if (source_ == AudioDataSource::ProgramOutput && targetProcessId_ != 0) {
      ComPtr<IAudioSessionManager2> sessionManager;
      hr = device_->Activate(
          IID_IAudioSessionManager2, CLSCTX_ALL, nullptr,
          reinterpret_cast<void **>(sessionManager.GetAddressOf()));
      if (SUCCEEDED(hr)) {
        sessionManager_ = std::move(sessionManager);
      }
    }

    audioEvent_.reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    if (!audioEvent_) {
      XX_LOGE("AudioStream: CreateEvent failed");
      CoTaskMemFree(mixFormat);
      return E_FAIL;
    }

    hr = audioClient_->SetEventHandle(audioEvent_.get());
    if (FAILED(hr)) {
      XX_LOGE("AudioStream: SetEventHandle failed, hr=0x{:08X}",
              static_cast<unsigned>(hr));
      CoTaskMemFree(mixFormat);
      return hr;
    }

    CoTaskMemFree(mixFormat);
    return S_OK;
  }

  void captureLoop() {
    HRESULT hr = audioClient_->Start();
    if (FAILED(hr)) {
      XX_LOGE("AudioStream: audioClient Start failed, hr=0x{:08X}",
              static_cast<unsigned>(hr));
      running_.store(false);
      return;
    }

    while (running_.load()) {
      DWORD waitResult = WaitForSingleObject(audioEvent_.get(), 200);
      if (waitResult != WAIT_OBJECT_0) {
        continue;
      }

      hr = readCaptureBuffer();
      if (FAILED(hr)) {
        XX_LOGE("AudioStream: readCaptureBuffer failed, hr=0x{:08X}",
                static_cast<unsigned>(hr));
        break;
      }
    }

    audioClient_->Stop();
  }

  HRESULT readCaptureBuffer() {
    UINT32 packetLength = 0;
    HRESULT hr = captureClient_->GetNextPacketSize(&packetLength);
    if (FAILED(hr)) {
      return hr;
    }

    while (packetLength != 0) {
      BYTE *data = nullptr;
      UINT32 numFrames = 0;
      DWORD flags = 0;

      hr = captureClient_->GetBuffer(&data, &numFrames, &flags, nullptr,
                                     nullptr);
      if (FAILED(hr)) {
        return hr;
      }

      if (numFrames > 0 && data != nullptr &&
          !(flags & AUDCLNT_BUFFERFLAGS_SILENT)) {
        size_t frameSize = channels_ * (bitsPerSample_ / 8);
        size_t dataSize = static_cast<size_t>(numFrames) * frameSize;

        if (source_ == AudioDataSource::ProgramOutput &&
            targetProcessId_ != 0 && sessionManager_) {
          if (!isTargetProcessActive()) {
            hr = captureClient_->ReleaseBuffer(numFrames);
            if (FAILED(hr)) {
              return hr;
            }
            hr = captureClient_->GetNextPacketSize(&packetLength);
            if (FAILED(hr)) {
              return hr;
            }
            continue;
          }
        }

        AudioData audioData;
        audioData.data.assign(data, data + dataSize);
        audioData.sampleRate = sampleRate_;
        audioData.channels = channels_;
        audioData.bitsPerSample = bitsPerSample_;
        audioData.source = source_;
        audioData.timestamp = std::chrono::steady_clock::now();
        audioData.processId = targetProcessId_;

        if (source_ == AudioDataSource::ProgramOutput &&
            targetProcessId_ != 0) {
          audioData.processName = getProcessName(targetProcessId_);
        }

        notifyListeners(audioData);
      }

      hr = captureClient_->ReleaseBuffer(numFrames);
      if (FAILED(hr)) {
        return hr;
      }

      hr = captureClient_->GetNextPacketSize(&packetLength);
      if (FAILED(hr)) {
        return hr;
      }
    }

    return S_OK;
  }

  bool isTargetProcessActive() {
    if (!sessionManager_) {
      return true;
    }

    ComPtr<IAudioSessionEnumerator> enumerator;
    HRESULT hr =
        sessionManager_->GetSessionEnumerator(enumerator.GetAddressOf());
    if (FAILED(hr)) {
      return true;
    }

    int sessionCount = 0;
    hr = enumerator->GetCount(&sessionCount);
    if (FAILED(hr)) {
      return true;
    }

    for (int i = 0; i < sessionCount; ++i) {
      ComPtr<IAudioSessionControl> sessionControl;
      hr = enumerator->GetSession(i, sessionControl.GetAddressOf());
      if (FAILED(hr)) {
        continue;
      }

      ComPtr<IAudioSessionControl2> sessionControl2;
      hr = sessionControl->QueryInterface(
          IID_IAudioSessionControl2,
          reinterpret_cast<void **>(sessionControl2.GetAddressOf()));
      if (FAILED(hr)) {
        continue;
      }

      DWORD processId = 0;
      hr = sessionControl2->GetProcessId(&processId);
      if (FAILED(hr)) {
        continue;
      }

      if (processId == targetProcessId_) {
        AudioSessionState state = AudioSessionStateExpired;
        sessionControl2->GetState(&state);
        return state == AudioSessionStateActive;
      }
    }

    return false;
  }

  static std::string getProcessName(uint32_t pid) {
    HANDLE hProcess =
        OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hProcess) {
      return {};
    }

    WCHAR name[MAX_PATH] = {};
    DWORD nameSize = MAX_PATH;
    if (QueryFullProcessImageNameW(hProcess, 0, name, &nameSize)) {
      CloseHandle(hProcess);
      std::wstring wname(name, nameSize);
      int size = WideCharToMultiByte(CP_UTF8, 0, wname.c_str(), -1, nullptr, 0,
                                     nullptr, nullptr);
      if (size > 0) {
        std::string result(size - 1, '\0');
        WideCharToMultiByte(CP_UTF8, 0, wname.c_str(), -1, result.data(), size,
                            nullptr, nullptr);
        return result;
      }
    }

    CloseHandle(hProcess);
    return {};
  }

  void notifyListeners(const AudioData &data) {
    std::lock_guard<std::mutex> lock(listenersMutex_);
    for (auto &listener : listeners_) {
      listener(data);
    }
  }

  void cleanupResources() {
    captureClient_.Reset();
    audioClient_.Reset();
    device_.Reset();
    enumerator_.Reset();
    sessionManager_.Reset();
    audioEvent_.reset();

    cleanupCom();
  }

  void cleanupCom() {
    if (comInitialized_) {
      CoUninitialize();
      comInitialized_ = false;
    }
  }

  struct HandleDeleter {
    void operator()(HANDLE h) const {
      if (h && h != INVALID_HANDLE_VALUE) {
        CloseHandle(h);
      }
    }
  };
  using UniqueHandle = std::unique_ptr<void, HandleDeleter>;

  std::atomic<bool> running_;
  AudioDataSource source_;
  uint32_t targetProcessId_;

  ComPtr<IMMDeviceEnumerator> enumerator_;
  ComPtr<IMMDevice> device_;
  ComPtr<IAudioClient> audioClient_;
  ComPtr<IAudioCaptureClient> captureClient_;
  ComPtr<IAudioSessionManager2> sessionManager_;
  UniqueHandle audioEvent_;

  uint32_t sampleRate_ = 0;
  uint16_t channels_ = 0;
  uint16_t bitsPerSample_ = 0;

  std::thread captureThread_;
  std::mutex listenersMutex_;
  std::vector<AudioStreamListener> listeners_;

  bool comInitialized_ = false;
};

#else

class AudioStream::Impl {
public:
  bool start(AudioDataSource, uint32_t) {
    XX_LOGE("AudioStream: not implemented on this platform");
    return false;
  }
  void stop() {}
  bool isRunning() const { return false; }
  void addListener(AudioStreamListener) {}
  void removeAllListeners() {}
};

#endif

AudioStream::AudioStream() : impl_(std::make_unique<Impl>()) {}

AudioStream::~AudioStream() = default;

bool AudioStream::start(AudioDataSource source, uint32_t targetProcessId) {
  return impl_->start(source, targetProcessId);
}

void AudioStream::stop() { impl_->stop(); }

bool AudioStream::isRunning() const { return impl_->isRunning(); }

void AudioStream::addListener(AudioStreamListener listener) {
  impl_->addListener(std::move(listener));
}

void AudioStream::removeAllListeners() { impl_->removeAllListeners(); }

} // namespace expand
} // namespace agentxx