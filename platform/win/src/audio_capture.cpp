#include "clipster/win/audio_capture.hpp"

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <propsys.h>

#if __has_include(<audioclientactivationparams.h>)
#include <audioclientactivationparams.h>
#define CLIPSTER_HAS_PROCESS_LOOPBACK 1
#else
#define CLIPSTER_HAS_PROCESS_LOOPBACK 0
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

#include "clipster/logging.hpp"
#include "clipster/util.hpp"
#include "clipster/win/str_util.hpp"

namespace clipster::win {

namespace {

// Shared WASAPI pull loop used by both capture flavours. Owns the event,
// the worker thread, and int16 -> float conversion when the mix format is
// not already float.
struct CaptureEngine {
  IAudioClient* client = nullptr;
  IAudioCaptureClient* capture = nullptr;
  HANDLE event = nullptr;
  int channels = 0;
  int sample_rate = 0;
  bool samples_are_int16 = false;
  // The process-loopback virtual device reports unreliable QPC positions
  // (zero or stream-relative on many builds), which would misplace audio
  // on the shared timeline. Stamp by arrival time instead — steady_clock
  // is QPC-based, the same clock video capture uses.
  bool use_arrival_timestamps = false;

  AudioSink sink;
  std::thread thread;
  std::atomic<bool> running{false};
  std::vector<float> convert_buf;

  ~CaptureEngine() {
    stop();
    if (capture) capture->Release();
    if (client) client->Release();
    if (event) CloseHandle(event);
  }

  bool start() {
    if (running.exchange(true)) {
      return true;
    }
    if (FAILED(client->Start())) {
      running = false;
      return false;
    }
    thread = std::thread([this] { run(); });
    return true;
  }

  void stop() {
    if (!running.exchange(false)) {
      return;
    }
    if (event) {
      SetEvent(event);  // wake the worker so it can observe running == false
    }
    if (thread.joinable()) {
      thread.join();
    }
    if (client) {
      client->Stop();
    }
  }

  void run() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    while (running.load(std::memory_order_relaxed)) {
      WaitForSingleObject(event, 200);
      UINT32 packet_frames = 0;
      while (running.load(std::memory_order_relaxed) &&
             SUCCEEDED(capture->GetNextPacketSize(&packet_frames)) && packet_frames > 0) {
        BYTE* data = nullptr;
        UINT32 frames = 0;
        DWORD flags = 0;
        UINT64 qpc_100ns = 0;
        if (FAILED(capture->GetBuffer(&data, &frames, &flags, nullptr, &qpc_100ns))) {
          break;
        }

        if (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) {
          log::debug("audio: data discontinuity (glitch or overflow)");
        }

        const size_t sample_count = static_cast<size_t>(frames) * channels;
        convert_buf.resize(sample_count);
        if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
          std::fill(convert_buf.begin(), convert_buf.end(), 0.0f);
        } else if (samples_are_int16) {
          const int16_t* src = reinterpret_cast<const int16_t*>(data);
          for (size_t i = 0; i < sample_count; ++i) {
            convert_buf[i] = static_cast<float>(src[i]) / 32768.0f;
          }
        } else {
          memcpy(convert_buf.data(), data, sample_count * sizeof(float));
        }

        AudioChunk chunk;
        chunk.samples = convert_buf.data();
        chunk.frame_count = static_cast<int>(frames);
        chunk.channels = channels;
        chunk.sample_rate = sample_rate;
        if (use_arrival_timestamps || qpc_100ns == 0) {
          const int64_t now_us = std::chrono::duration_cast<std::chrono::microseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count();
          chunk.timestamp_us = now_us - static_cast<int64_t>(frames) * 1'000'000 / sample_rate;
        } else {
          chunk.timestamp_us = static_cast<int64_t>(qpc_100ns / 10);
        }
        sink(chunk);

        capture->ReleaseBuffer(frames);
      }
    }
    CoUninitialize();
  }
};

// KSDATAFORMAT_SUBTYPE_* spelled out locally to avoid dragging in
// ksmedia.h + GUID instantiation quirks.
constexpr GUID kSubtypeIeeeFloat = {
    0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
constexpr GUID kSubtypePcm = {
    0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

bool is_float_format(const WAVEFORMATEX* wf) {
  if (wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
    return true;
  }
  if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
    const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wf);
    return IsEqualGUID(ext->SubFormat, kSubtypeIeeeFloat);
  }
  return false;
}

bool is_int16_format(const WAVEFORMATEX* wf) {
  if (wf->wBitsPerSample != 16) {
    return false;
  }
  if (wf->wFormatTag == WAVE_FORMAT_PCM) {
    return true;
  }
  if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
    const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wf);
    return IsEqualGUID(ext->SubFormat, kSubtypePcm);
  }
  return false;
}

// One full second of buffer: the worker polls every 200 ms (loopback
// events are unreliable on some drivers), so the buffer must comfortably
// absorb several missed polls without dropping audio.
constexpr REFERENCE_TIME kBufferDuration100ns = 10'000'000;

bool finish_engine_setup(CaptureEngine& eng, std::string* error) {
  eng.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (!eng.event || FAILED(eng.client->SetEventHandle(eng.event))) {
    if (error) *error = "SetEventHandle failed";
    return false;
  }
  if (FAILED(eng.client->GetService(__uuidof(IAudioCaptureClient),
                                    reinterpret_cast<void**>(&eng.capture)))) {
    if (error) *error = "GetService(IAudioCaptureClient) failed";
    return false;
  }
  return true;
}

}  // namespace

// --- DesktopLoopbackCapture --------------------------------------------------

struct DesktopLoopbackCapture::Impl {
  CaptureEngine engine;
};

DesktopLoopbackCapture::DesktopLoopbackCapture() : impl_(std::make_unique<Impl>()) {}
DesktopLoopbackCapture::~DesktopLoopbackCapture() = default;

std::unique_ptr<DesktopLoopbackCapture> DesktopLoopbackCapture::create(AudioSink sink,
                                                                       std::string* error) {
  auto fail = [&](const char* msg) -> std::unique_ptr<DesktopLoopbackCapture> {
    if (error) *error = msg;
    return nullptr;
  };

  auto cap = std::unique_ptr<DesktopLoopbackCapture>(new DesktopLoopbackCapture());
  CaptureEngine& eng = cap->impl_->engine;
  eng.sink = std::move(sink);

  IMMDeviceEnumerator* enumerator = nullptr;
  if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                              __uuidof(IMMDeviceEnumerator),
                              reinterpret_cast<void**>(&enumerator)))) {
    return fail("CoCreateInstance(MMDeviceEnumerator) failed — is COM initialized?");
  }

  IMMDevice* device = nullptr;
  const HRESULT dev_hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
  enumerator->Release();
  if (FAILED(dev_hr)) {
    return fail("no default audio output device");
  }

  const HRESULT act_hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                          reinterpret_cast<void**>(&eng.client));
  device->Release();
  if (FAILED(act_hr)) {
    return fail("IAudioClient activation failed");
  }

  WAVEFORMATEX* wf = nullptr;
  if (FAILED(eng.client->GetMixFormat(&wf))) {
    return fail("GetMixFormat failed");
  }
  eng.channels = wf->nChannels;
  eng.sample_rate = static_cast<int>(wf->nSamplesPerSec);
  eng.samples_are_int16 = is_int16_format(wf);
  if (!eng.samples_are_int16 && !is_float_format(wf)) {
    CoTaskMemFree(wf);
    return fail("unsupported mix format (expected float32 or int16)");
  }

  const HRESULT init_hr = eng.client->Initialize(
      AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
      kBufferDuration100ns, 0, wf, nullptr);
  CoTaskMemFree(wf);
  if (FAILED(init_hr)) {
    return fail("IAudioClient::Initialize (loopback) failed");
  }

  if (!finish_engine_setup(eng, error)) {
    return nullptr;
  }
  log::info("audio: desktop loopback ready ({} ch @ {} Hz)", eng.channels, eng.sample_rate);
  return cap;
}

void DesktopLoopbackCapture::start() { impl_->engine.start(); }
void DesktopLoopbackCapture::stop() { impl_->engine.stop(); }
int DesktopLoopbackCapture::sample_rate() const { return impl_->engine.sample_rate; }
int DesktopLoopbackCapture::channels() const { return impl_->engine.channels; }

// --- MicrophoneCapture -------------------------------------------------------

namespace {

// PKEY_Device_FriendlyName, spelled out to avoid GUID-instantiation
// header ceremony.
constexpr PROPERTYKEY kFriendlyNameKey = {
    {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};

std::string device_friendly_name(IMMDevice* device) {
  IPropertyStore* store = nullptr;
  if (FAILED(device->OpenPropertyStore(STGM_READ, &store))) {
    return {};
  }
  PROPVARIANT value;
  PropVariantInit(&value);
  std::string name;
  if (SUCCEEDED(store->GetValue(kFriendlyNameKey, &value)) && value.vt == VT_LPWSTR) {
    name = narrow(value.pwszVal);
  }
  PropVariantClear(&value);
  store->Release();
  return name;
}

// Default communications mic (what Discord uses), or the first active
// capture endpoint whose friendly name contains `wanted`.
IMMDevice* find_capture_device(const std::string& wanted) {
  IMMDeviceEnumerator* enumerator = nullptr;
  if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                              __uuidof(IMMDeviceEnumerator),
                              reinterpret_cast<void**>(&enumerator)))) {
    return nullptr;
  }

  IMMDevice* device = nullptr;
  if (!wanted.empty() && wanted != "default") {
    IMMDeviceCollection* collection = nullptr;
    if (SUCCEEDED(enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &collection))) {
      UINT count = 0;
      collection->GetCount(&count);
      const std::string wanted_lower = util::to_lower_ascii(wanted);
      for (UINT i = 0; i < count && !device; ++i) {
        IMMDevice* candidate = nullptr;
        if (FAILED(collection->Item(i, &candidate))) {
          continue;
        }
        if (util::to_lower_ascii(device_friendly_name(candidate)).find(wanted_lower) !=
            std::string::npos) {
          device = candidate;  // transfer ownership
        } else {
          candidate->Release();
        }
      }
      collection->Release();
    }
    if (!device) {
      log::warn("audio: microphone '{}' not found — using the default device", wanted);
    }
  }
  if (!device) {
    enumerator->GetDefaultAudioEndpoint(eCapture, eCommunications, &device);
  }
  enumerator->Release();
  return device;
}

}  // namespace

std::vector<std::string> list_capture_devices() {
  std::vector<std::string> names;
  IMMDeviceEnumerator* enumerator = nullptr;
  if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                              __uuidof(IMMDeviceEnumerator),
                              reinterpret_cast<void**>(&enumerator)))) {
    return names;
  }
  IMMDeviceCollection* collection = nullptr;
  if (SUCCEEDED(enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &collection))) {
    UINT count = 0;
    collection->GetCount(&count);
    for (UINT i = 0; i < count; ++i) {
      IMMDevice* device = nullptr;
      if (SUCCEEDED(collection->Item(i, &device))) {
        std::string name = device_friendly_name(device);
        if (!name.empty()) {
          names.push_back(std::move(name));
        }
        device->Release();
      }
    }
    collection->Release();
  }
  enumerator->Release();
  return names;
}

struct MicrophoneCapture::Impl {
  CaptureEngine engine;
};

MicrophoneCapture::MicrophoneCapture() : impl_(std::make_unique<Impl>()) {}
MicrophoneCapture::~MicrophoneCapture() = default;

std::unique_ptr<MicrophoneCapture> MicrophoneCapture::create(const std::string& device_name,
                                                             AudioSink sink,
                                                             std::string* error) {
  auto fail = [&](const char* msg) -> std::unique_ptr<MicrophoneCapture> {
    if (error) *error = msg;
    return nullptr;
  };

  auto cap = std::unique_ptr<MicrophoneCapture>(new MicrophoneCapture());
  CaptureEngine& eng = cap->impl_->engine;
  eng.sink = std::move(sink);

  IMMDevice* device = find_capture_device(device_name);
  if (!device) {
    return fail("no microphone available");
  }
  log::info("audio: microphone '{}'", device_friendly_name(device));

  const HRESULT act_hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                          reinterpret_cast<void**>(&eng.client));
  device->Release();
  if (FAILED(act_hr)) {
    return fail("microphone IAudioClient activation failed");
  }

  WAVEFORMATEX* wf = nullptr;
  if (FAILED(eng.client->GetMixFormat(&wf))) {
    return fail("microphone GetMixFormat failed");
  }
  eng.channels = wf->nChannels;
  eng.sample_rate = static_cast<int>(wf->nSamplesPerSec);
  eng.samples_are_int16 = is_int16_format(wf);
  if (!eng.samples_are_int16 && !is_float_format(wf)) {
    CoTaskMemFree(wf);
    return fail("unsupported microphone format (expected float32 or int16)");
  }

  const HRESULT init_hr =
      eng.client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                             kBufferDuration100ns, 0, wf, nullptr);
  CoTaskMemFree(wf);
  if (FAILED(init_hr)) {
    return fail("microphone IAudioClient::Initialize failed");
  }

  if (!finish_engine_setup(eng, error)) {
    return nullptr;
  }
  log::info("audio: microphone ready ({} ch @ {} Hz)", eng.channels, eng.sample_rate);
  return cap;
}

void MicrophoneCapture::start() { impl_->engine.start(); }
void MicrophoneCapture::stop() { impl_->engine.stop(); }
int MicrophoneCapture::sample_rate() const { return impl_->engine.sample_rate; }
int MicrophoneCapture::channels() const { return impl_->engine.channels; }

// --- ProcessLoopbackCapture --------------------------------------------------

#if CLIPSTER_HAS_PROCESS_LOOPBACK

namespace {

// Synchronous wrapper around ActivateAudioInterfaceAsync. Heap-allocated
// with real refcounting: the OS may invoke ActivateCompleted on a worker
// thread after our wait() times out, so the handler (and the activation
// params blob it owns) must outlive the in-flight operation, not the
// caller's stack frame. IAgileObject keeps the callback free-threaded.
class ActivationWaiter : public IActivateAudioInterfaceCompletionHandler, public IAgileObject {
 public:
  explicit ActivationWaiter(const AUDIOCLIENT_ACTIVATION_PARAMS& params)
      : params_(params), done_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {
    prop_.vt = VT_BLOB;
    prop_.blob.cbSize = sizeof(params_);
    prop_.blob.pBlobData = reinterpret_cast<BYTE*>(&params_);
  }

  ULONG STDMETHODCALLTYPE AddRef() override {
    return static_cast<ULONG>(refs_.fetch_add(1, std::memory_order_relaxed) + 1);
  }
  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG left = static_cast<ULONG>(refs_.fetch_sub(1, std::memory_order_acq_rel) - 1);
    if (left == 0) {
      delete this;
    }
    return left;
  }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** obj) override {
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IActivateAudioInterfaceCompletionHandler)) {
      *obj = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
      AddRef();
      return S_OK;
    }
    if (riid == __uuidof(IAgileObject)) {
      *obj = static_cast<IAgileObject*>(this);
      AddRef();
      return S_OK;
    }
    *obj = nullptr;
    return E_NOINTERFACE;
  }

  HRESULT STDMETHODCALLTYPE
  ActivateCompleted(IActivateAudioInterfaceAsyncOperation* op) override {
    IUnknown* unknown = nullptr;
    HRESULT activate_hr = E_FAIL;
    if (SUCCEEDED(op->GetActivateResult(&activate_hr, &unknown)) && SUCCEEDED(activate_hr) &&
        unknown) {
      unknown->QueryInterface(__uuidof(IAudioClient), reinterpret_cast<void**>(&client_));
      unknown->Release();
    }
    SetEvent(done_);
    return S_OK;
  }

  PROPVARIANT* activation_prop() { return &prop_; }

  // Returns the activated client (caller owns the reference) or nullptr on
  // timeout/failure. Safe to call once; a late callback after timeout only
  // touches this heap object, which its own reference keeps alive.
  IAudioClient* wait(DWORD timeout_ms) {
    if (WaitForSingleObject(done_, timeout_ms) != WAIT_OBJECT_0) {
      return nullptr;
    }
    IAudioClient* out = client_;
    client_ = nullptr;
    return out;
  }

 private:
  ~ActivationWaiter() {
    if (client_) {
      client_->Release();  // activated but abandoned by a timed-out wait()
    }
    if (done_) {
      CloseHandle(done_);
    }
  }

  std::atomic<long> refs_{1};
  AUDIOCLIENT_ACTIVATION_PARAMS params_;
  PROPVARIANT prop_{};
  HANDLE done_ = nullptr;
  IAudioClient* client_ = nullptr;
};

}  // namespace

struct ProcessLoopbackCapture::Impl {
  CaptureEngine engine;
};

ProcessLoopbackCapture::ProcessLoopbackCapture() : impl_(std::make_unique<Impl>()) {}
ProcessLoopbackCapture::~ProcessLoopbackCapture() = default;

bool ProcessLoopbackCapture::is_supported() {
  // Process loopback shipped with Windows 10 2004 (build 19041).
  using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
  const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
  if (!ntdll) {
    return false;
  }
  const auto rtl_get_version =
      reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion"));
  if (!rtl_get_version) {
    return false;
  }
  RTL_OSVERSIONINFOW info{};
  info.dwOSVersionInfoSize = sizeof(info);
  if (rtl_get_version(&info) != 0) {
    return false;
  }
  return info.dwMajorVersion > 10 ||
         (info.dwMajorVersion == 10 && info.dwBuildNumber >= 19041);
}

std::unique_ptr<ProcessLoopbackCapture> ProcessLoopbackCapture::create(DWORD pid, Mode mode,
                                                                       AudioSink sink,
                                                                       std::string* error) {
  auto fail = [&](const std::string& msg) -> std::unique_ptr<ProcessLoopbackCapture> {
    if (error) *error = msg;
    return nullptr;
  };

  if (!is_supported()) {
    return fail("process loopback requires Windows 10 2004 or newer");
  }

  auto cap = std::unique_ptr<ProcessLoopbackCapture>(new ProcessLoopbackCapture());
  CaptureEngine& eng = cap->impl_->engine;
  eng.sink = std::move(sink);

  AUDIOCLIENT_ACTIVATION_PARAMS params{};
  params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
  params.ProcessLoopbackParams.TargetProcessId = pid;
  params.ProcessLoopbackParams.ProcessLoopbackMode =
      mode == Mode::IncludeTree ? PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE
                                : PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE;

  ActivationWaiter* waiter = new ActivationWaiter(params);  // refcounted, self-deleting
  IActivateAudioInterfaceAsyncOperation* op = nullptr;
  const HRESULT hr =
      ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient),
                                  waiter->activation_prop(), waiter, &op);
  if (FAILED(hr)) {
    waiter->Release();
    return fail("ActivateAudioInterfaceAsync failed");
  }
  eng.client = waiter->wait(5000);
  waiter->Release();  // a late callback still holds its own reference
  if (op) {
    op->Release();
  }
  if (!eng.client) {
    return fail("process loopback activation failed or timed out");
  }

  // Process loopback has no device mix format: the client dictates it.
  WAVEFORMATEX wf{};
  wf.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
  wf.nChannels = 2;
  wf.nSamplesPerSec = 48000;
  wf.wBitsPerSample = 32;
  wf.nBlockAlign = wf.nChannels * wf.wBitsPerSample / 8;
  wf.nAvgBytesPerSec = wf.nSamplesPerSec * wf.nBlockAlign;
  eng.channels = wf.nChannels;
  eng.sample_rate = static_cast<int>(wf.nSamplesPerSec);
  eng.samples_are_int16 = false;
  eng.use_arrival_timestamps = true;  // see CaptureEngine comment

  if (FAILED(eng.client->Initialize(
          AUDCLNT_SHAREMODE_SHARED,
          AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
          kBufferDuration100ns, 0, &wf, nullptr))) {
    return fail("IAudioClient::Initialize (process loopback) failed");
  }

  if (!finish_engine_setup(eng, error)) {
    return nullptr;
  }
  log::info("audio: process loopback ready (pid {}, {})", pid,
            mode == Mode::IncludeTree ? "include tree" : "exclude tree");
  return cap;
}

void ProcessLoopbackCapture::start() { impl_->engine.start(); }
void ProcessLoopbackCapture::stop() { impl_->engine.stop(); }

#else  // !CLIPSTER_HAS_PROCESS_LOOPBACK

struct ProcessLoopbackCapture::Impl {};
ProcessLoopbackCapture::ProcessLoopbackCapture() = default;
ProcessLoopbackCapture::~ProcessLoopbackCapture() = default;
bool ProcessLoopbackCapture::is_supported() { return false; }
std::unique_ptr<ProcessLoopbackCapture> ProcessLoopbackCapture::create(DWORD, Mode, AudioSink,
                                                                       std::string* error) {
  if (error) {
    *error = "built without <audioclientactivationparams.h> (Windows SDK too old)";
  }
  return nullptr;
}
void ProcessLoopbackCapture::start() {}
void ProcessLoopbackCapture::stop() {}

#endif  // CLIPSTER_HAS_PROCESS_LOOPBACK

}  // namespace clipster::win
