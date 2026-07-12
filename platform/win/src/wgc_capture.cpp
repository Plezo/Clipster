#include "clipster/win/wgc_capture.hpp"

#include <winrt/base.h>

#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Graphics.DirectX.h>

#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <d3d11.h>
#include <dwmapi.h>
#include <dxgi1_2.h>

#include <algorithm>
#include <cstdlib>
#include <mutex>

#include "clipster/logging.hpp"

namespace clipster::win {

namespace winrt_wgc = winrt::Windows::Graphics::Capture;
namespace winrt_dx = winrt::Windows::Graphics::DirectX;
namespace winrt_d3d = winrt::Windows::Graphics::DirectX::Direct3D11;

struct WgcCapture::Impl {
  winrt::com_ptr<ID3D11Device> d3d_device;
  winrt::com_ptr<ID3D11DeviceContext> d3d_context;
  winrt_d3d::IDirect3DDevice winrt_device{nullptr};

  winrt_wgc::GraphicsCaptureItem item{nullptr};
  winrt_wgc::Direct3D11CaptureFramePool frame_pool{nullptr};
  winrt_wgc::GraphicsCaptureSession session{nullptr};
  winrt_wgc::Direct3D11CaptureFramePool::FrameArrived_revoker frame_arrived;
  winrt_wgc::GraphicsCaptureItem::Closed_revoker item_closed;
  std::function<void()> on_closed;

  winrt::com_ptr<ID3D11Texture2D> staging;
  D3D11_TEXTURE2D_DESC staging_desc{};
  winrt::Windows::Graphics::SizeInt32 pool_size{};

  FrameSink sink;
  std::mutex frame_mutex;
  bool running = false;
  HWND hwnd = nullptr;
  bool client_area_only = true;

  void on_frame(const winrt_wgc::Direct3D11CaptureFrame& frame);

  // Offset and size of the window's client area within the captured
  // texture, in physical pixels. Returns false if it cannot be computed
  // (capture then falls back to the full window).
  bool client_crop(int content_w, int content_h, int* off_x, int* off_y, int* w, int* h) const;
};

WgcCapture::WgcCapture() : impl_(std::make_unique<Impl>()) {}

WgcCapture::~WgcCapture() { stop(); }

bool WgcCapture::is_supported() { return winrt_wgc::GraphicsCaptureSession::IsSupported(); }

std::unique_ptr<WgcCapture> WgcCapture::create_for_window(HWND hwnd, FrameSink sink,
                                                          std::string* error,
                                                          bool client_area_only) {
  auto fail = [&](const std::string& msg) -> std::unique_ptr<WgcCapture> {
    if (error) *error = msg;
    return nullptr;
  };

  if (!is_supported()) {
    return fail("Windows.Graphics.Capture is not supported on this system (Windows 10 1903+)");
  }

  auto cap = std::unique_ptr<WgcCapture>(new WgcCapture());
  Impl& im = *cap->impl_;
  im.sink = std::move(sink);
  im.hwnd = hwnd;
  im.client_area_only = client_area_only;

  UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
  if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr, 0,
                               D3D11_SDK_VERSION, im.d3d_device.put(), nullptr,
                               im.d3d_context.put()))) {
    return fail("D3D11CreateDevice failed");
  }

  // Wrap the DXGI device as a WinRT IDirect3DDevice for the frame pool.
  winrt::com_ptr<IInspectable> inspectable;
  if (FAILED(CreateDirect3D11DeviceFromDXGIDevice(im.d3d_device.as<IDXGIDevice>().get(),
                                                  inspectable.put()))) {
    return fail("CreateDirect3D11DeviceFromDXGIDevice failed");
  }
  im.winrt_device = inspectable.as<winrt_d3d::IDirect3DDevice>();

  // Create the capture item for the target window.
  auto interop = winrt::get_activation_factory<winrt_wgc::GraphicsCaptureItem>()
                     .as<IGraphicsCaptureItemInterop>();
  if (FAILED(interop->CreateForWindow(hwnd, winrt::guid_of<winrt_wgc::GraphicsCaptureItem>(),
                                      winrt::put_abi(im.item)))) {
    return fail("CreateForWindow failed — is the window still open?");
  }

  im.pool_size = im.item.Size();
  im.frame_pool = winrt_wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(
      im.winrt_device, winrt_dx::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, im.pool_size);
  im.session = im.frame_pool.CreateCaptureSession(im.item);

  // Remove the yellow capture border where the OS allows it (2104+).
  try {
    if (winrt::Windows::Foundation::Metadata::ApiInformation::IsPropertyPresent(
            L"Windows.Graphics.Capture.GraphicsCaptureSession", L"IsBorderRequired")) {
      im.session.IsBorderRequired(false);
    }
  } catch (const winrt::hresult_error&) {
    // Needs user consent on some builds; the border is cosmetic, carry on.
  }

  im.frame_arrived = im.frame_pool.FrameArrived(
      winrt::auto_revoke, [impl = cap->impl_.get()](const auto& pool, const auto&) {
        if (auto frame = pool.TryGetNextFrame()) {
          impl->on_frame(frame);
        }
      });

  im.item_closed =
      im.item.Closed(winrt::auto_revoke, [impl = cap->impl_.get()](const auto&, const auto&) {
        std::function<void()> callback;
        {
          std::lock_guard lock(impl->frame_mutex);
          if (impl->running) {
            callback = impl->on_closed;
          }
        }
        if (callback) {
          callback();
        }
      });

  return cap;
}

void WgcCapture::set_on_closed(std::function<void()> callback) {
  std::lock_guard lock(impl_->frame_mutex);
  impl_->on_closed = std::move(callback);
}

void WgcCapture::Impl::on_frame(const winrt_wgc::Direct3D11CaptureFrame& frame) {
  std::lock_guard lock(frame_mutex);
  if (!running) {
    return;
  }

  const auto content_size = frame.ContentSize();
  if (content_size.Width != pool_size.Width || content_size.Height != pool_size.Height) {
    // Window was resized: recreate the pool at the new size and wait for
    // the next frame.
    pool_size = content_size;
    frame_pool.Recreate(winrt_device, winrt_dx::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2,
                        pool_size);
    return;
  }

  winrt::com_ptr<ID3D11Texture2D> texture;
  auto access = frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::
                                       IDirect3DDxgiInterfaceAccess>();
  if (FAILED(access->GetInterface(winrt::guid_of<ID3D11Texture2D>(), texture.put_void()))) {
    return;
  }

  D3D11_TEXTURE2D_DESC desc{};
  texture->GetDesc(&desc);

  // (Re)create the CPU staging texture when the size changes.
  if (!staging || staging_desc.Width != desc.Width || staging_desc.Height != desc.Height) {
    staging = nullptr;
    D3D11_TEXTURE2D_DESC sd = desc;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    sd.MiscFlags = 0;
    if (FAILED(d3d_device->CreateTexture2D(&sd, nullptr, staging.put()))) {
      log::error("wgc: failed to create staging texture {}x{}", desc.Width, desc.Height);
      return;
    }
    staging_desc = sd;
  }

  // TODO(perf): this GPU->CPU readback costs one copy per frame. The
  // zero-copy path (hand the D3D11 texture to the encoder via AV_PIX_FMT_D3D11)
  // is the planned optimization once the pipeline is proven end to end.
  d3d_context->CopyResource(staging.get(), texture.get());
  D3D11_MAPPED_SUBRESOURCE mapped{};
  if (FAILED(d3d_context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped))) {
    return;
  }

  CapturedFrame out;
  out.width = std::min<int>(content_size.Width, static_cast<int>(desc.Width));
  out.height = std::min<int>(content_size.Height, static_cast<int>(desc.Height));
  out.stride = static_cast<int>(mapped.RowPitch);

  int off_x = 0;
  int off_y = 0;
  if (client_area_only) {
    int cw = 0;
    int ch = 0;
    if (client_crop(out.width, out.height, &off_x, &off_y, &cw, &ch)) {
      out.width = cw;
      out.height = ch;
    } else {
      off_x = off_y = 0;
    }
  }
  out.data = static_cast<const uint8_t*>(mapped.pData) +
             static_cast<size_t>(off_y) * out.stride + static_cast<size_t>(off_x) * 4;
  // SystemRelativeTime is QPC-based, in 100 ns ticks.
  out.timestamp_us = frame.SystemRelativeTime().count() / 10;
  sink(out);

  d3d_context->Unmap(staging.get(), 0);
}

bool WgcCapture::Impl::client_crop(int content_w, int content_h, int* off_x, int* off_y, int* w,
                                   int* h) const {
  // The rect queries must see physical pixels regardless of process DPI
  // awareness, or the crop lands in the wrong place on scaled displays.
  const DPI_AWARENESS_CONTEXT prev =
      SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  RECT window_rect{};
  RECT client_rect{};
  POINT client_origin{0, 0};
  const bool ok = GetWindowRect(hwnd, &window_rect) && GetClientRect(hwnd, &client_rect) &&
                  ClientToScreen(hwnd, &client_origin);
  RECT ext_rect{};
  const bool have_ext = SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS,
                                                        &ext_rect, sizeof(ext_rect)));
  SetThreadDpiAwarenessContext(prev);
  if (!ok || client_rect.right <= 0 || client_rect.bottom <= 0) {
    return false;
  }

  // The captured texture may correspond to the raw window rect (with
  // invisible resize borders) or to the DWM visible bounds, depending on
  // the OS build; pick whichever matches the captured size best.
  RECT base = window_rect;
  if (have_ext) {
    const int wr_w = window_rect.right - window_rect.left;
    const int ext_w = ext_rect.right - ext_rect.left;
    if (std::abs(ext_w - content_w) < std::abs(wr_w - content_w)) {
      base = ext_rect;
    }
  }

  const int x = std::clamp<int>(client_origin.x - base.left, 0, content_w);
  const int y = std::clamp<int>(client_origin.y - base.top, 0, content_h);
  const int cw = std::min<int>(client_rect.right, content_w - x);
  const int ch = std::min<int>(client_rect.bottom, content_h - y);
  if (cw < 16 || ch < 16) {
    return false;  // degenerate (minimized or mid-resize) — keep full frame
  }
  *off_x = x;
  *off_y = y;
  *w = cw;
  *h = ch;
  return true;
}

void WgcCapture::start() {
  std::lock_guard lock(impl_->frame_mutex);
  if (!impl_->running) {
    impl_->running = true;
    impl_->session.StartCapture();
    log::info("wgc: capture started ({}x{})", impl_->pool_size.Width, impl_->pool_size.Height);
  }
}

void WgcCapture::stop() {
  {
    std::lock_guard lock(impl_->frame_mutex);
    if (!impl_->running) {
      return;
    }
    impl_->running = false;
  }
  impl_->frame_arrived.revoke();
  impl_->item_closed.revoke();
  if (impl_->session) {
    impl_->session.Close();
  }
  if (impl_->frame_pool) {
    impl_->frame_pool.Close();
  }
  log::info("wgc: capture stopped");
}

}  // namespace clipster::win
