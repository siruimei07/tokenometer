#include "CaptureRenderer.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <string>

#include <d3dcompiler.h>
#include <microsoft.ui.xaml.media.dxinterop.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

using namespace winrt;
using namespace winrt::Windows::Graphics;
using namespace winrt::Windows::Graphics::Capture;
using namespace winrt::Windows::Graphics::DirectX;
using namespace winrt::Windows::Graphics::DirectX::Direct3D11;
using namespace winrt::Microsoft::UI::Xaml::Controls;

namespace
{
    std::filesystem::path ShaderPath()
    {
        std::array<wchar_t, 32768> modulePath{};
        auto const length = GetModuleFileNameW(nullptr, modulePath.data(), static_cast<DWORD>(modulePath.size()));
        if (length && length < modulePath.size())
        {
            auto const besideExe = std::filesystem::path(modulePath.data()).parent_path() / L"Glass.hlsl";
            if (std::filesystem::exists(besideExe))
            {
                return besideExe;
            }
        }
        return std::filesystem::path(__FILE__).parent_path() / L"Glass.hlsl";
    }

    com_ptr<ID3DBlob> CompileShader(char const* entryPoint, char const* target)
    {
        com_ptr<ID3DBlob> bytecode;
        com_ptr<ID3DBlob> errors;
        UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
        flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
        flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif
        auto const path = ShaderPath();
        auto const result = D3DCompileFromFile(
            path.c_str(),
            nullptr,
            D3D_COMPILE_STANDARD_FILE_INCLUDE,
            entryPoint,
            target,
            flags,
            0,
            bytecode.put(),
            errors.put());

        if (FAILED(result))
        {
            std::string message = "D3DCompileFromFile failed: ";
            message += path.string();
            if (errors)
            {
                message += "\n";
                message.append(
                    static_cast<char const*>(errors->GetBufferPointer()),
                    errors->GetBufferSize());
            }
            throw hresult_error(result, to_hstring(message));
        }
        return bytecode;
    }

}

std::shared_ptr<CaptureRenderer> CaptureRenderer::Create(HWND window, SwapChainPanel const& panel)
{
    auto renderer = std::shared_ptr<CaptureRenderer>(new CaptureRenderer(window, panel));
    renderer->Initialize();
    return renderer;
}

CaptureRenderer::CaptureRenderer(HWND window, SwapChainPanel panel) :
    m_window(window),
    m_panel(std::move(panel))
{
}

CaptureRenderer::~CaptureRenderer()
{
    Stop();
}

void CaptureRenderer::Initialize()
{
    RECT client{};
    check_bool(GetClientRect(m_window, &client));
    auto const width = std::max<LONG>(client.right - client.left, 1);
    auto const height = std::max<LONG>(client.bottom - client.top, 1);

    CreateDevice();
    CreateSwapChain(static_cast<uint32_t>(width), static_cast<uint32_t>(height));
    AttachSwapChain();
    CreatePipeline();
    CreateSizeResources(static_cast<uint32_t>(width), static_cast<uint32_t>(height));
    StartCapture();
}

void CaptureRenderer::CreateDevice()
{
    constexpr std::array featureLevels{ D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    D3D_FEATURE_LEVEL selected{};
    auto result = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        featureLevels.data(),
        static_cast<UINT>(featureLevels.size()),
        D3D11_SDK_VERSION,
        m_device.put(),
        &selected,
        m_context.put());

    if (FAILED(result))
    {
        check_hresult(D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_WARP,
            nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            featureLevels.data(),
            static_cast<UINT>(featureLevels.size()),
            D3D11_SDK_VERSION,
            m_device.put(),
            &selected,
            m_context.put()));
    }

    auto multithread = m_context.as<ID3D11Multithread>();
    multithread->SetMultithreadProtected(TRUE);

    auto const dxgiDevice = m_device.as<IDXGIDevice>();
    com_ptr<::IInspectable> inspectable;
    check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.get(), inspectable.put()));
    m_winrtDevice = inspectable.as<IDirect3DDevice>();
}

void CaptureRenderer::CreateSwapChain(uint32_t width, uint32_t height)
{
    auto const dxgiDevice = m_device.as<IDXGIDevice>();
    com_ptr<IDXGIAdapter> adapter;
    check_hresult(dxgiDevice->GetAdapter(adapter.put()));

    com_ptr<IDXGIFactory2> factory;
    check_hresult(adapter->GetParent(__uuidof(IDXGIFactory2), factory.put_void()));

    DXGI_SWAP_CHAIN_DESC1 description{};
    description.Width = width;
    description.Height = height;
    description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    description.BufferCount = 2;
    description.Scaling = DXGI_SCALING_STRETCH;
    description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    description.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;

    check_hresult(factory->CreateSwapChainForComposition(
        m_device.get(),
        &description,
        nullptr,
        m_swapChain.put()));
}

void CaptureRenderer::AttachSwapChain()
{
    auto const nativePanel = m_panel.as<ISwapChainPanelNative>();
    check_hresult(nativePanel->SetSwapChain(m_swapChain.get()));
}

void CaptureRenderer::CreatePipeline()
{
    auto const vertex = CompileShader("VSMain", "vs_5_0");
    check_hresult(m_device->CreateVertexShader(
        vertex->GetBufferPointer(), vertex->GetBufferSize(), nullptr, m_vertexShader.put()));

    auto createPixelShader = [this](char const* entryPoint, com_ptr<ID3D11PixelShader>& shader)
    {
        auto const bytecode = CompileShader(entryPoint, "ps_5_0");
        check_hresult(m_device->CreatePixelShader(
            bytecode->GetBufferPointer(), bytecode->GetBufferSize(), nullptr, shader.put()));
    };
    createPixelShader("PrefilterPS", m_prefilterShader);
    createPixelShader("BlurHPS", m_blurHShader);
    createPixelShader("BlurVPS", m_blurVShader);
    createPixelShader("GlassPS", m_glassShader);

    D3D11_SAMPLER_DESC sampler{};
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    check_hresult(m_device->CreateSamplerState(&sampler, m_sampler.put()));

    static_assert(sizeof(ShaderParams) % 16 == 0);
    D3D11_BUFFER_DESC constantBuffer{};
    constantBuffer.ByteWidth = sizeof(ShaderParams);
    constantBuffer.Usage = D3D11_USAGE_DYNAMIC;
    constantBuffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    constantBuffer.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    check_hresult(m_device->CreateBuffer(&constantBuffer, nullptr, m_paramsBuffer.put()));
}

CaptureRenderer::RenderTexture CaptureRenderer::CreateRenderTexture(uint32_t width, uint32_t height) const
{
    RenderTexture result;
    D3D11_TEXTURE2D_DESC description{};
    description.Width = width;
    description.Height = height;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    check_hresult(m_device->CreateTexture2D(&description, nullptr, result.texture.put()));
    check_hresult(m_device->CreateRenderTargetView(result.texture.get(), nullptr, result.target.put()));
    check_hresult(m_device->CreateShaderResourceView(result.texture.get(), nullptr, result.view.put()));
    return result;
}

void CaptureRenderer::CreateSizeResources(uint32_t width, uint32_t height)
{
    m_width = std::max(width, 1u);
    m_height = std::max(height, 1u);
    m_effectWidth = std::max((m_width + 1u) / 2u, 1u);
    m_effectHeight = std::max((m_height + 1u) / 2u, 1u);
    m_scratchA = CreateRenderTexture(m_effectWidth, m_effectHeight);
    m_scratchB = CreateRenderTexture(m_effectWidth, m_effectHeight);
}

void CaptureRenderer::StartCapture()
{
    m_monitor = MonitorFromWindow(m_window, MONITOR_DEFAULTTOPRIMARY);
    auto const interop = get_activation_factory<GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
    check_hresult(interop->CreateForMonitor(
        m_monitor,
        guid_of<GraphicsCaptureItem>(),
        put_abi(m_captureItem)));

    m_captureSize = m_captureItem.Size();
    m_framePool = Direct3D11CaptureFramePool::CreateFreeThreaded(
        m_winrtDevice,
        DirectXPixelFormat::B8G8R8A8UIntNormalized,
        2,
        m_captureSize);
    m_captureSession = m_framePool.CreateCaptureSession(m_captureItem);
    m_captureSession.IsCursorCaptureEnabled(false);

    auto weak = weak_from_this();
    m_frameArrived = m_framePool.FrameArrived(
        [weak](Direct3D11CaptureFramePool const& sender, auto const&)
        {
            if (auto renderer = weak.lock())
            {
                renderer->OnFrame(sender);
            }
        });
    m_captureSession.StartCapture();
}

void CaptureRenderer::OnFrame(Direct3D11CaptureFramePool const& sender)
{
    try
    {
        if (m_stopping.load(std::memory_order_acquire))
        {
            return;
        }

        std::scoped_lock lock(m_renderMutex);
        if (m_stopping.load(std::memory_order_relaxed))
        {
            return;
        }

        auto frame = sender.TryGetNextFrame();
        if (!frame)
        {
            return;
        }

        auto const frameSize = frame.ContentSize();
        if (frameSize.Width != m_captureSize.Width || frameSize.Height != m_captureSize.Height)
        {
            m_captureSize = frameSize;
            m_framePool.Recreate(
                m_winrtDevice,
                DirectXPixelFormat::B8G8R8A8UIntNormalized,
                2,
                m_captureSize);
        }

        auto const access = frame.Surface().as<
            ::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
        com_ptr<ID3D11Texture2D> capturedTexture;
        check_hresult(access->GetInterface(__uuidof(ID3D11Texture2D), capturedTexture.put_void()));

        Render(capturedTexture);
    }
    catch (hresult_error const& error)
    {
        OutputDebugStringW(error.message().c_str());
        OutputDebugStringW(L"\nTokenometer capture frame failed.\n");
    }
    catch (...)
    {
        OutputDebugStringW(L"Tokenometer capture frame failed.\n");
    }
}

CaptureRenderer::ShaderParams CaptureRenderer::CropParams() const
{
    POINT clientOrigin{};
    check_bool(ClientToScreen(m_window, &clientOrigin));

    MONITORINFO monitorInfo{ sizeof(monitorInfo) };
    check_bool(GetMonitorInfoW(m_monitor, &monitorInfo));

    auto const captureWidth = static_cast<float>(std::max(m_captureSize.Width, 1));
    auto const captureHeight = static_cast<float>(std::max(m_captureSize.Height, 1));
    ShaderParams params{};
    params.sourceScale[0] = static_cast<float>(m_width) / captureWidth;
    params.sourceScale[1] = static_cast<float>(m_height) / captureHeight;
    params.sourceOffset[0] = static_cast<float>(clientOrigin.x - monitorInfo.rcMonitor.left) / captureWidth;
    params.sourceOffset[1] = static_cast<float>(clientOrigin.y - monitorInfo.rcMonitor.top) / captureHeight;
    params.texelSize[0] = 1.0f / static_cast<float>(m_effectWidth);
    params.texelSize[1] = 1.0f / static_cast<float>(m_effectHeight);
    params.tint[0] = 0.045f;
    params.tint[1] = 0.047f;
    params.tint[2] = 0.044f;
    params.tint[3] = 1.0f;
    params.outputSize[0] = static_cast<float>(m_width);
    params.outputSize[1] = static_cast<float>(m_height);
    params.cornerRadius = 18.0f * static_cast<float>(GetDpiForWindow(m_window)) / 96.0f;
    params.borderWidth = 1.5f;
    return params;
}

void CaptureRenderer::Render(com_ptr<ID3D11Texture2D> const& capturedTexture)
{
    com_ptr<ID3D11ShaderResourceView> capturedView;
    check_hresult(m_device->CreateShaderResourceView(capturedTexture.get(), nullptr, capturedView.put()));

    auto params = CropParams();
    DrawPass(
        m_prefilterShader.get(),
        capturedView.get(),
        m_scratchA.target.get(),
        m_effectWidth,
        m_effectHeight,
        params);

    params.sourceScale[0] = 1.0f;
    params.sourceScale[1] = 1.0f;
    params.sourceOffset[0] = 0.0f;
    params.sourceOffset[1] = 0.0f;
    DrawPass(
        m_blurHShader.get(),
        m_scratchA.view.get(),
        m_scratchB.target.get(),
        m_effectWidth,
        m_effectHeight,
        params);
    DrawPass(
        m_blurVShader.get(),
        m_scratchB.view.get(),
        m_scratchA.target.get(),
        m_effectWidth,
        m_effectHeight,
        params);

    params = CropParams();
    com_ptr<ID3D11Texture2D> backBuffer;
    check_hresult(m_swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), backBuffer.put_void()));
    com_ptr<ID3D11RenderTargetView> backBufferTarget;
    check_hresult(m_device->CreateRenderTargetView(backBuffer.get(), nullptr, backBufferTarget.put()));
    DrawPass(
        m_glassShader.get(),
        m_scratchA.view.get(),
        backBufferTarget.get(),
        m_width,
        m_height,
        params);

    check_hresult(m_swapChain->Present(1, 0));
    m_presentedFrames.fetch_add(1, std::memory_order_relaxed);
}

void CaptureRenderer::DrawPass(
    ID3D11PixelShader* shader,
    ID3D11ShaderResourceView* source0,
    ID3D11RenderTargetView* target,
    uint32_t viewportWidth,
    uint32_t viewportHeight,
    ShaderParams const& params)
{
    D3D11_MAPPED_SUBRESOURCE mapped{};
    check_hresult(m_context->Map(m_paramsBuffer.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));
    std::memcpy(mapped.pData, &params, sizeof(params));
    m_context->Unmap(m_paramsBuffer.get(), 0);

    D3D11_VIEWPORT viewport{};
    viewport.Width = static_cast<float>(viewportWidth);
    viewport.Height = static_cast<float>(viewportHeight);
    viewport.MaxDepth = 1.0f;

    ID3D11ShaderResourceView* sources[]{ source0 };
    ID3D11SamplerState* samplers[]{ m_sampler.get() };
    ID3D11Buffer* constants[]{ m_paramsBuffer.get() };

    m_context->RSSetViewports(1, &viewport);
    m_context->OMSetRenderTargets(1, &target, nullptr);
    m_context->IASetInputLayout(nullptr);
    m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_context->VSSetShader(m_vertexShader.get(), nullptr, 0);
    m_context->PSSetShader(shader, nullptr, 0);
    m_context->PSSetShaderResources(0, 1, sources);
    m_context->PSSetSamplers(0, 1, samplers);
    m_context->PSSetConstantBuffers(0, 1, constants);
    m_context->Draw(3, 0);

    ID3D11ShaderResourceView* emptySources[]{ nullptr };
    m_context->PSSetShaderResources(0, 1, emptySources);
    m_context->OMSetRenderTargets(0, nullptr, nullptr);
}

void CaptureRenderer::Resize(uint32_t pixelWidth, uint32_t pixelHeight)
{
    pixelWidth = std::max(pixelWidth, 1u);
    pixelHeight = std::max(pixelHeight, 1u);
    std::scoped_lock lock(m_renderMutex);
    if (!m_swapChain || (pixelWidth == m_width && pixelHeight == m_height))
    {
        return;
    }

    m_context->ClearState();
    m_scratchA = {};
    m_scratchB = {};
    check_hresult(m_swapChain->ResizeBuffers(
        2,
        pixelWidth,
        pixelHeight,
        DXGI_FORMAT_B8G8R8A8_UNORM,
        0));
    CreateSizeResources(pixelWidth, pixelHeight);
}

void CaptureRenderer::Stop()
{
    if (m_stopping.exchange(true, std::memory_order_acq_rel))
    {
        return;
    }

    if (m_framePool && m_frameArrived.value)
    {
        m_framePool.FrameArrived(m_frameArrived);
        m_frameArrived = {};
    }

    std::scoped_lock lock(m_renderMutex);
    if (m_captureSession)
    {
        m_captureSession.Close();
        m_captureSession = nullptr;
    }
    if (m_framePool)
    {
        m_framePool.Close();
        m_framePool = nullptr;
    }
    m_captureItem = nullptr;

    if (m_panel)
    {
        auto const nativePanel = m_panel.as<ISwapChainPanelNative>();
        nativePanel->SetSwapChain(nullptr);
        m_panel = nullptr;
    }
}
