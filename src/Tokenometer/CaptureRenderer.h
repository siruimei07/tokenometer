#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>

#include <d3d11_4.h>
#include <dxgi1_3.h>
#ifdef GetCurrentTime
#undef GetCurrentTime
#endif
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

class CaptureRenderer final : public std::enable_shared_from_this<CaptureRenderer>
{
public:
    static std::shared_ptr<CaptureRenderer> Create(
        HWND window,
        winrt::Microsoft::UI::Xaml::Controls::SwapChainPanel const& panel);

    ~CaptureRenderer();

    CaptureRenderer(CaptureRenderer const&) = delete;
    CaptureRenderer& operator=(CaptureRenderer const&) = delete;

    void Resize(uint32_t pixelWidth, uint32_t pixelHeight);
    void Stop();
    [[nodiscard]] uint64_t PresentedFrames() const noexcept
    {
        return m_presentedFrames.load(std::memory_order_relaxed);
    }

private:
    struct RenderTexture
    {
        winrt::com_ptr<ID3D11Texture2D> texture;
        winrt::com_ptr<ID3D11RenderTargetView> target;
        winrt::com_ptr<ID3D11ShaderResourceView> view;
    };

    struct ShaderParams
    {
        float sourceScale[2];
        float sourceOffset[2];
        float texelSize[2];
        float padding[2];
        float tint[4];
        float outputSize[2];
        float cornerRadius;
        float borderWidth;
    };

    CaptureRenderer(HWND window, winrt::Microsoft::UI::Xaml::Controls::SwapChainPanel panel);

    void Initialize();
    void CreateDevice();
    void CreateSwapChain(uint32_t width, uint32_t height);
    void AttachSwapChain();
    void CreatePipeline();
    void CreateSizeResources(uint32_t width, uint32_t height);
    RenderTexture CreateRenderTexture(uint32_t width, uint32_t height) const;
    void StartCapture();
    void OnFrame(winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool const& sender);
    void Render(winrt::com_ptr<ID3D11Texture2D> const& capturedTexture);
    void DrawPass(
        ID3D11PixelShader* shader,
        ID3D11ShaderResourceView* source0,
        ID3D11ShaderResourceView* source1,
        ID3D11RenderTargetView* target,
        ShaderParams const& params);
    ShaderParams CropParams() const;

    HWND m_window{};
    winrt::Microsoft::UI::Xaml::Controls::SwapChainPanel m_panel{ nullptr };

    winrt::com_ptr<ID3D11Device> m_device;
    winrt::com_ptr<ID3D11DeviceContext> m_context;
    winrt::com_ptr<IDXGISwapChain1> m_swapChain;
    winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice m_winrtDevice{ nullptr };

    winrt::com_ptr<ID3D11VertexShader> m_vertexShader;
    winrt::com_ptr<ID3D11PixelShader> m_prefilterShader;
    winrt::com_ptr<ID3D11PixelShader> m_blurHShader;
    winrt::com_ptr<ID3D11PixelShader> m_blurVShader;
    winrt::com_ptr<ID3D11PixelShader> m_glassShader;
    winrt::com_ptr<ID3D11SamplerState> m_sampler;
    winrt::com_ptr<ID3D11Buffer> m_paramsBuffer;

    RenderTexture m_scratchA;
    RenderTexture m_scratchB;
    uint32_t m_width{};
    uint32_t m_height{};

    HMONITOR m_monitor{};
    winrt::Windows::Graphics::Capture::GraphicsCaptureItem m_captureItem{ nullptr };
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool m_framePool{ nullptr };
    winrt::Windows::Graphics::Capture::GraphicsCaptureSession m_captureSession{ nullptr };
    winrt::Windows::Graphics::SizeInt32 m_captureSize{};
    winrt::event_token m_frameArrived{};

    std::atomic_bool m_stopping{ false };
    std::atomic_uint64_t m_presentedFrames{};
    std::mutex m_renderMutex;
};
