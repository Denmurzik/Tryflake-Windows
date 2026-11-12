#include "Gpu.h"

#include "SnowPS.h"
#include "SnowVS.h"

using namespace snow;

// ================================================================= сетка

namespace {

struct Color {
    float r, g, b, a;
};

inline void push(std::vector<GpuVertex>& out, double x, double y, float u, float v, Color c, float style, float rotation) {
    out.push_back(GpuVertex{static_cast<float>(x), static_cast<float>(y), u, v, c.r, c.g, c.b, c.a, style, rotation});
}

inline void triangle(std::vector<GpuVertex>& out, Point a, Point b, Point c, Color ca, Color cb, Color cc) {
    push(out, a.x, a.y, 0, 0, ca, 2, 0);
    push(out, b.x, b.y, 0, 0, cb, 2, 0);
    push(out, c.x, c.y, 0, 0, cc, 2, 0);
}

inline void quad(std::vector<GpuVertex>& out, double x, double y, double w, double h, Color c) {
    Point a(x, y), b(x + w, y), cc(x + w, y + h), d(x, y + h);
    triangle(out, a, b, cc, c, c, c);
    triangle(out, a, cc, d, c, c, c);
}

inline void disc(std::vector<GpuVertex>& out, Point p, double r, Color c, float style, float rotation = 0) {
    static const float corners[6][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, -1}, {1, 1}, {-1, 1}};
    for (const auto& k : corners) push(out, p.x + k[0] * r, p.y + k[1] * r, k[0], k[1], c, style, rotation);
}

}  // namespace

void SnowMesh::build(const SnowWorld& world, double opacity, std::vector<GpuVertex>& out) {
    out.clear();
    out.reserve(world.particles().size() * 6 + 12000);
    const float o = static_cast<float>(opacity);
    const Color white{0.97f, 0.985f, 1.f, o};
    const Color ice{0.79f, 0.86f, 0.93f, o};
    if (world.configuration.accumulation) {
        for (const auto& surface : world.surfaces()) {
            const auto& s = surface.geometry;
            const int end = static_cast<int>(surface.depths.size()) - 1;
            for (int i = 0; i < end; ++i) {
                if (!s.exposed[i] || !s.exposed[i + 1]) continue;
                const double left = s.x + i * SurfaceGeometry::spacing;
                const double right = std::min(s.x + s.width, left + SurfaceGeometry::spacing);
                if (right < 0 || left > world.width || !(surface.depths[i] + surface.depths[i + 1] > 0.15)) continue;
                const double leftTaper = s.id == 0 ? 1 : std::min(1.0, std::min(i, end - i) / 2.0);
                const double rightTaper = s.id == 0 ? 1 : std::min(1.0, std::min(i + 1, end - i - 1) / 2.0);
                Point a(left, s.y - surface.depths[i] * leftTaper);
                Point b(right, s.y - surface.depths[i + 1] * rightTaper);
                Point c(right, s.y + 0.7);
                Point d(left, s.y + 0.7);
                triangle(out, a, b, c, white, white, ice);
                triangle(out, a, c, d, white, ice, ice);
            }
        }
    }
    for (const auto& chunk : world.chunks()) {
        const float co = static_cast<float>(opacity * chunk.opacity());
        const Color cw{0.97f, 0.985f, 1.f, co};
        const Color ci{0.78f, 0.85f, 0.92f, co};
        for (size_t k = 1; k < chunk.columns.size(); ++k) {
            const auto& left = chunk.columns[k - 1];
            const auto& right = chunk.columns[k];
            Point a = chunk.transformed(Point(left.x, left.top));
            Point b = chunk.transformed(Point(right.x, right.top));
            Point c = chunk.transformed(Point(right.x, right.bottom));
            Point d = chunk.transformed(Point(left.x, left.bottom));
            triangle(out, a, b, c, cw, cw, ci);
            triangle(out, a, c, d, cw, ci, ci);
        }
    }
    for (const auto& p : world.particles()) {
        const Color c{0.96f, 0.98f, 1.f, static_cast<float>(p.opacity * opacity)};
        disc(out, p.position, p.radius, c, p.radius > 2.4 && !p.debris ? 3.f : 0.f, static_cast<float>(p.phase));
    }
    for (const auto& ball : world.balls()) {
        disc(out, Point(ball.position.x + 0.4, ball.position.y + 1.2), ball.radius + 1,
             Color{0.24f, 0.35f, 0.43f, static_cast<float>(0.12 * opacity)}, 0);
        disc(out, ball.position, ball.radius, white, 1);
    }
    for (const auto& head : world.snowmanHeads()) {
        const Point p = head.position;
        const double r = head.radius;
        const Color coal{0.13f, 0.18f, 0.22f, o};
        const Color carrot{0.91f, 0.43f, 0.18f, o};
        const Color scarf{0.64f, 0.22f, 0.22f, o};
        disc(out, Point(p.x - r * 0.29, p.y - r * 0.22), std::max(1.1, r * 0.09), coal, 1);
        disc(out, Point(p.x + r * 0.29, p.y - r * 0.22), std::max(1.1, r * 0.09), coal, 1);
        triangle(out, Point(p.x - r * 0.04, p.y - r * 0.04), Point(p.x + r * 0.69, p.y + r * 0.18),
                 Point(p.x - r * 0.04, p.y + r * 0.23), carrot, carrot, carrot);
        for (int i = -1; i <= 1; ++i) {
            disc(out, Point(p.x + i * r * 0.2, p.y + r * (0.48 - std::abs(i) * 0.05)), std::max(0.8, r * 0.055), coal, 1);
        }
        quad(out, p.x - r * 0.75, p.y + r * 0.7, r * 1.5, std::max(3.0, r * 0.26), scarf);
        quad(out, p.x + r * 0.25, p.y + r * 0.85, std::max(3.0, r * 0.25), r * 0.8, scarf);
    }
}

// ================================================================= устройство

bool GpuContext::create(std::wstring& error) {
    destroy();
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                        D3D_FEATURE_LEVEL_10_0};
    const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, ARRAYSIZE(levels),
                                   D3D11_SDK_VERSION, &device, nullptr, &context);
    warp = false;
    if (FAILED(hr)) {
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, levels, ARRAYSIZE(levels),
                               D3D11_SDK_VERSION, &device, nullptr, &context);
        warp = SUCCEEDED(hr);
    }
    if (FAILED(hr)) {
        error = L"Не удалось создать устройство Direct3D 11: " + hrMessage(hr);
        return false;
    }
    device.As(&dxgiDevice);
    dxgiDevice->SetMaximumFrameLatency(1);
    ComPtr<IDXGIAdapter> adapter;
    dxgiDevice->GetAdapter(&adapter);
    DXGI_ADAPTER_DESC desc{};
    adapter->GetDesc(&desc);
    adapterName = desc.Description;
    adapter->GetParent(IID_PPV_ARGS(&factory));

    if (FAILED(hr = DCompositionCreateDevice(dxgiDevice.Get(), IID_PPV_ARGS(&dcomp)))) {
        error = L"DirectComposition недоступен: " + hrMessage(hr);
        destroy();
        return false;
    }
    D2D1_FACTORY_OPTIONS options{};
    if (FAILED(hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &options,
                                      reinterpret_cast<void**>(d2dFactory.GetAddressOf()))) ||
        FAILED(hr = d2dFactory->CreateDevice(dxgiDevice.Get(), &d2dDevice))) {
        error = L"Direct2D недоступен: " + hrMessage(hr);
        destroy();
        return false;
    }
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                        reinterpret_cast<IUnknown**>(dwrite.GetAddressOf()));
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));

    if (FAILED(hr = device->CreateVertexShader(g_vsMain, sizeof(g_vsMain), nullptr, &vs_)) ||
        FAILED(hr = device->CreatePixelShader(g_psMain, sizeof(g_psMain), nullptr, &ps_))) {
        error = L"Не удалось загрузить шейдер снега: " + hrMessage(hr);
        destroy();
        return false;
    }
    const D3D11_INPUT_ELEMENT_DESC elements[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT, 0, 32, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    device->CreateInputLayout(elements, ARRAYSIZE(elements), g_vsMain, sizeof(g_vsMain), &layout_);

    D3D11_BLEND_DESC blend{};
    blend.RenderTarget[0].BlendEnable = TRUE;
    blend.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    blend.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blend.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    device->CreateBlendState(&blend, &blend_);

    D3D11_RASTERIZER_DESC raster{};
    raster.FillMode = D3D11_FILL_SOLID;
    raster.CullMode = D3D11_CULL_NONE;
    raster.DepthClipEnable = TRUE;
    device->CreateRasterizerState(&raster, &raster_);

    D3D11_BUFFER_DESC cb{};
    cb.ByteWidth = 16;
    cb.Usage = D3D11_USAGE_DYNAMIC;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    device->CreateBuffer(&cb, nullptr, &constants_);
    return true;
}

void GpuContext::destroy() {
    vertexBuffer_.Reset();
    vertexCapacity_ = 0;
    constants_.Reset();
    raster_.Reset();
    blend_.Reset();
    layout_.Reset();
    ps_.Reset();
    vs_.Reset();
    wic.Reset();
    dwrite.Reset();
    d2dDevice.Reset();
    d2dFactory.Reset();
    dcomp.Reset();
    factory.Reset();
    dxgiDevice.Reset();
    if (context) context->ClearState();
    context.Reset();
    device.Reset();
}

bool GpuContext::deviceLost() const { return device && FAILED(device->GetDeviceRemovedReason()); }

void GpuContext::draw(ID3D11RenderTargetView* rtv, UINT pixelWidth, UINT pixelHeight, float viewportWidth,
                      float viewportHeight, const std::vector<GpuVertex>& vertices, const float clear[4]) {
    context->ClearRenderTargetView(rtv, clear);
    if (vertices.empty()) return;
    const UINT count = static_cast<UINT>(vertices.size());
    if (count > vertexCapacity_) {
        vertexCapacity_ = std::max<UINT>(16384, count * 2);
        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = vertexCapacity_ * sizeof(GpuVertex);
        desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        vertexBuffer_.Reset();
        if (FAILED(device->CreateBuffer(&desc, nullptr, &vertexBuffer_))) {
            vertexCapacity_ = 0;
            return;
        }
    }
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(vertexBuffer_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return;
    memcpy(mapped.pData, vertices.data(), count * sizeof(GpuVertex));
    context->Unmap(vertexBuffer_.Get(), 0);
    if (SUCCEEDED(context->Map(constants_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        const float data[4] = {viewportWidth, viewportHeight, 0, 0};
        memcpy(mapped.pData, data, sizeof(data));
        context->Unmap(constants_.Get(), 0);
    }
    D3D11_VIEWPORT viewport{0, 0, static_cast<float>(pixelWidth), static_cast<float>(pixelHeight), 0, 1};
    const UINT stride = sizeof(GpuVertex), offset = 0;
    ID3D11Buffer* vb = vertexBuffer_.Get();
    ID3D11Buffer* cbs = constants_.Get();
    context->OMSetRenderTargets(1, &rtv, nullptr);
    context->OMSetBlendState(blend_.Get(), nullptr, 0xFFFFFFFF);
    context->RSSetState(raster_.Get());
    context->RSSetViewports(1, &viewport);
    context->IASetInputLayout(layout_.Get());
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
    context->VSSetShader(vs_.Get(), nullptr, 0);
    context->VSSetConstantBuffers(0, 1, &cbs);
    context->PSSetShader(ps_.Get(), nullptr, 0);
    context->Draw(count, 0);
    ID3D11RenderTargetView* none = nullptr;
    context->OMSetRenderTargets(1, &none, nullptr);
}

bool GpuContext::renderPNG(const SnowWorld& world, const std::wstring& path, std::wstring& error) {
    const UINT w = static_cast<UINT>(world.width), h = static_cast<UINT>(world.height);
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = w;
    desc.Height = h;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> texture, staging;
    ComPtr<ID3D11RenderTargetView> rtv;
    HRESULT hr = device->CreateTexture2D(&desc, nullptr, &texture);
    if (SUCCEEDED(hr)) hr = device->CreateRenderTargetView(texture.Get(), nullptr, &rtv);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (SUCCEEDED(hr)) hr = device->CreateTexture2D(&desc, nullptr, &staging);
    if (FAILED(hr)) {
        error = L"Не удалось создать текстуру: " + hrMessage(hr);
        return false;
    }
    std::vector<GpuVertex> vertices;
    SnowMesh::build(world, 1, vertices);
    const float clear[4] = {0.045f, 0.075f, 0.09f, 1};
    draw(rtv.Get(), w, h, static_cast<float>(world.width), static_cast<float>(world.height), vertices, clear);
    context->CopyResource(staging.Get(), texture.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(hr = context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
        error = L"Не удалось прочитать кадр: " + hrMessage(hr);
        return false;
    }
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    hr = wic ? S_OK : E_NOINTERFACE;
    if (SUCCEEDED(hr)) hr = wic->CreateStream(&stream);
    if (SUCCEEDED(hr)) hr = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
    if (SUCCEEDED(hr)) hr = wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
    if (SUCCEEDED(hr)) hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    if (SUCCEEDED(hr)) hr = encoder->CreateNewFrame(&frame, nullptr);
    if (SUCCEEDED(hr)) hr = frame->Initialize(nullptr);
    if (SUCCEEDED(hr)) hr = frame->SetSize(w, h);
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    if (SUCCEEDED(hr)) hr = frame->SetPixelFormat(&format);
    if (SUCCEEDED(hr))
        hr = frame->WritePixels(h, mapped.RowPitch, mapped.RowPitch * h, static_cast<BYTE*>(mapped.pData));
    if (SUCCEEDED(hr)) hr = frame->Commit();
    if (SUCCEEDED(hr)) hr = encoder->Commit();
    context->Unmap(staging.Get(), 0);
    if (FAILED(hr)) {
        error = L"Не удалось сохранить PNG: " + hrMessage(hr);
        return false;
    }
    return true;
}
