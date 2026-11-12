#pragma once

#include "Common.h"
#include "../core/SnowCore.h"

struct GpuVertex {
    float x, y;
    float u, v;
    float r, g, b, a;
    float style, rotation;
};
static_assert(sizeof(GpuVertex) == 40);

namespace SnowMesh {
void build(const snow::SnowWorld& world, double opacity, std::vector<GpuVertex>& out);
}

/// Общие для всего приложения устройства: D3D11, DirectComposition, Direct2D, DirectWrite, WIC.
class GpuContext {
public:
    bool create(std::wstring& error);
    void destroy();
    bool alive() const { return device != nullptr; }

    /// Рисует вершины в RTV. viewport — логический размер сцены.
    void draw(ID3D11RenderTargetView* rtv, UINT pixelWidth, UINT pixelHeight, float viewportWidth,
              float viewportHeight, const std::vector<GpuVertex>& vertices, const float clear[4]);
    bool renderPNG(const snow::SnowWorld& world, const std::wstring& path, std::wstring& error);
    bool deviceLost() const;

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGIDevice1> dxgiDevice;
    ComPtr<IDXGIFactory2> factory;
    ComPtr<IDCompositionDevice> dcomp;
    ComPtr<ID2D1Factory1> d2dFactory;
    ComPtr<ID2D1Device> d2dDevice;
    ComPtr<IDWriteFactory> dwrite;
    ComPtr<IWICImagingFactory> wic;
    std::wstring adapterName;
    bool warp = false;

private:
    ComPtr<ID3D11VertexShader> vs_;
    ComPtr<ID3D11PixelShader> ps_;
    ComPtr<ID3D11InputLayout> layout_;
    ComPtr<ID3D11BlendState> blend_;
    ComPtr<ID3D11RasterizerState> raster_;
    ComPtr<ID3D11Buffer> constants_;
    ComPtr<ID3D11Buffer> vertexBuffer_;
    UINT vertexCapacity_ = 0;
};
