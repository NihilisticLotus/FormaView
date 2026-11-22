#include "video_output.hpp"
#include <QtQuick/qsgtexture_platform.h>
#include <dxgi1_6.h>
#include <rhi/qrhi.h>
#include <QtGui/private/qrhid3d11_p.h>
#include <wrl/client.h>

namespace videoOutput {
bool hdrDesktop(QQuickWindow *window){
    if(!window)return false;
    auto monitor=MonitorFromWindow(reinterpret_cast<HWND>(window->winId()),MONITOR_DEFAULTTONEAREST);
    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    if(FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))return false;
    for(UINT i=0;;++i){
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        if(factory->EnumAdapters1(i,&adapter)!=S_OK)break;
        for(UINT j=0;;++j){
            Microsoft::WRL::ComPtr<IDXGIOutput> output;
            if(adapter->EnumOutputs(j,&output)!=S_OK)break;
            Microsoft::WRL::ComPtr<IDXGIOutput6> ext;
            DXGI_OUTPUT_DESC1 desc{};
            if(SUCCEEDED(output.As(&ext))&&SUCCEEDED(ext->GetDesc1(&desc))&&desc.Monitor==monitor)
                return desc.BitsPerColor>=10&&desc.ColorSpace==DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
        }
    }
    return false;
}
QVariantMap presentationInfo(QQuickWindow *window) {
    QVariantMap info;
    if (!window->swapChain() || window->rendererInterface()->graphicsApi() != QSGRendererInterface::Direct3D11)
        return info;
    // Qt 6.12 is pinned by the host. Access the backend only during scene synchronization.
    const auto *rhiSwap = static_cast<const QD3D11SwapChain *>(window->swapChain());
    auto *swap = rhiSwap->swapChain;
    DXGI_SWAP_CHAIN_DESC desc{};
    if (!swap || FAILED(swap->GetDesc(&desc)))
        return info;
    info.insert("buffers", desc.BufferCount);
    info.insert("swap-width", desc.BufferDesc.Width);
    info.insert("swap-height", desc.BufferDesc.Height);
    const auto format = desc.BufferDesc.Format;
    if (format == DXGI_FORMAT_B8G8R8A8_UNORM) info.insert("swap-format", "BGRA8");
    else if (format == DXGI_FORMAT_R8G8B8A8_UNORM) info.insert("swap-format", "RGBA8");
    else if (format == DXGI_FORMAT_R10G10B10A2_UNORM) info.insert("swap-format", "RGB10A2");
    else if (format == DXGI_FORMAT_R16G16B16A16_FLOAT) info.insert("swap-format", "RGBA16F");
    const char *effect = nullptr;
    switch (desc.SwapEffect) {
    case DXGI_SWAP_EFFECT_FLIP_DISCARD: effect = "Flip Discard"; break;
    case DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL: effect = "Flip Sequential"; break;
    case DXGI_SWAP_EFFECT_DISCARD: effect = "Discard"; break;
    case DXGI_SWAP_EFFECT_SEQUENTIAL: effect = "Sequential"; break;
    default: break;
    }
    if (effect) info.insert("swap-effect", QString::fromLatin1(effect));
    UINT submitted{};
    if (SUCCEEDED(swap->GetLastPresentCount(&submitted))) info.insert("submitted", submitted);
    DXGI_FRAME_STATISTICS stats{};
    if (SUCCEEDED(swap->GetFrameStatistics(&stats))) {
        info.insert("presented", stats.PresentCount);
        info.insert("refresh-count", stats.PresentRefreshCount);
        info.insert("sync-count", stats.SyncRefreshCount);
    }
    return info;
}
bool pqSwapchain(QQuickWindow *window){
    auto *swap=window->swapChain();
    return swap&&swap->format()==QRhiSwapChain::HDR10;
}
QSGTexture *importTexture(QQuickWindow *window,ID3D11Texture2D *texture,QSize size,bool pq){
    if(!pq)return QNativeInterface::QSGD3D11Texture::fromNative(texture,window,size);
    auto *rhi=window->rhi();if(!rhi)return nullptr;
    auto *native=rhi->newTexture(QRhiTexture::RGB10A2,size);
    if(!native->createFrom({quint64(reinterpret_cast<quintptr>(texture)),0})){delete native;return nullptr;}
    auto *result=window->createTextureFromRhiTexture(native);
    if(!result)delete native;
    return result;
}
}
