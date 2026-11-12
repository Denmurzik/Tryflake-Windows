#include "Icons.h"

namespace {

using D2D1::Point2F;

// Стиль штриха кэшируется; держим ссылку на фабрику, чтобы кэш не пережил её подмену.
ComPtr<ID2D1StrokeStyle> roundStroke(ID2D1Factory* factory) {
    static ComPtr<ID2D1Factory> cachedFactory;
    static ComPtr<ID2D1StrokeStyle> cached;
    if (cached && cachedFactory.Get() == factory) return cached;
    ComPtr<ID2D1StrokeStyle> style;
    D2D1_STROKE_STYLE_PROPERTIES p = D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                                                 D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND);
    if (FAILED(factory->CreateStrokeStyle(p, nullptr, 0, &style))) return nullptr;
    cachedFactory = factory;
    cached = style;
    return style;
}

void line(ID2D1RenderTarget* rt, float x0, float y0, float x1, float y1, ID2D1Brush* b, float w, ID2D1StrokeStyle* s) {
    rt->DrawLine(Point2F(x0, y0), Point2F(x1, y1), b, w, s);
}

void snowflake(ID2D1RenderTarget* rt, D2D1_POINT_2F c, float r, ID2D1Brush* b, float w, ID2D1StrokeStyle* s) {
    for (int arm = 0; arm < 6; ++arm) {
        const float a = static_cast<float>(arm * 3.14159265 / 3 - 3.14159265 / 2);
        const float ca = std::cos(a), sa = std::sin(a);
        line(rt, c.x, c.y, c.x + ca * r, c.y + sa * r, b, w, s);
        const float bx = c.x + ca * r * 0.58f, by = c.y + sa * r * 0.58f;
        for (int side = -1; side <= 1; side += 2) {
            const float ba = a + side * 0.75f;
            line(rt, bx, by, bx + std::cos(ba) * r * 0.32f, by + std::sin(ba) * r * 0.32f, b, w, s);
        }
    }
}

ComPtr<ID2D1PathGeometry> cloud(ID2D1Factory* factory, D2D1_POINT_2F c, float s) {
    // Облако в квадрате s×s, низ на c.y + 0.12s.
    ComPtr<ID2D1PathGeometry> path;
    factory->CreatePathGeometry(&path);
    ComPtr<ID2D1GeometrySink> sink;
    path->Open(&sink);
    const float left = c.x - s * 0.42f, right = c.x + s * 0.42f, bottom = c.y + s * 0.12f;
    sink->BeginFigure(Point2F(left + s * 0.16f, bottom), D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddLine(Point2F(right - s * 0.14f, bottom));
    sink->AddArc(D2D1::ArcSegment(Point2F(right - s * 0.2f, c.y - s * 0.2f), D2D1::SizeF(s * 0.17f, s * 0.17f), 0,
                                  D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE, D2D1_ARC_SIZE_LARGE));
    sink->AddArc(D2D1::ArcSegment(Point2F(left + s * 0.2f, c.y - s * 0.1f), D2D1::SizeF(s * 0.25f, s * 0.25f), 0,
                                  D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
    sink->AddArc(D2D1::ArcSegment(Point2F(left + s * 0.16f, bottom), D2D1::SizeF(s * 0.12f, s * 0.12f), 0,
                                  D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE, D2D1_ARC_SIZE_LARGE));
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    sink->Close();
    return path;
}

}  // namespace

void Icons::draw(ID2D1RenderTarget* rt, ID2D1Factory* factory, Kind kind, D2D1_POINT_2F c, float s, ID2D1Brush* b,
                 float w) {
    auto stroke = roundStroke(factory);
    ID2D1StrokeStyle* st = stroke.Get();
    switch (kind) {
        case Kind::snowflake:
            snowflake(rt, c, s * 0.5f, b, w, st);
            break;
        case Kind::cloudSnow: {
            auto path = cloud(factory, Point2F(c.x, c.y - s * 0.12f), s);
            rt->DrawGeometry(path.Get(), b, w, st);
            for (int i = -1; i <= 1; ++i) {
                const float x = c.x + i * s * 0.22f, y = c.y + s * (i == 0 ? 0.36f : 0.27f);
                rt->FillEllipse(D2D1::Ellipse(Point2F(x, y), w * 0.85f, w * 0.85f), b);
            }
            break;
        }
        case Kind::windSnow: {
            ComPtr<ID2D1PathGeometry> path;
            factory->CreatePathGeometry(&path);
            ComPtr<ID2D1GeometrySink> sink;
            path->Open(&sink);
            const float l = c.x - s * 0.45f;
            const float ys[3] = {c.y - s * 0.24f, c.y, c.y + s * 0.24f};
            const float lens[3] = {0.62f, 0.8f, 0.5f};
            for (int i = 0; i < 3; ++i) {
                const float end = l + s * lens[i];
                sink->BeginFigure(Point2F(l, ys[i]), D2D1_FIGURE_BEGIN_HOLLOW);
                sink->AddLine(Point2F(end, ys[i]));
                sink->AddArc(D2D1::ArcSegment(Point2F(end, ys[i] - s * 0.16f), D2D1::SizeF(s * 0.08f, s * 0.08f), 0,
                                              D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE, D2D1_ARC_SIZE_LARGE));
                sink->EndFigure(D2D1_FIGURE_END_OPEN);
            }
            sink->Close();
            rt->DrawGeometry(path.Get(), b, w, st);
            rt->FillEllipse(D2D1::Ellipse(Point2F(c.x + s * 0.42f, c.y + s * 0.12f), w * 0.85f, w * 0.85f), b);
            rt->FillEllipse(D2D1::Ellipse(Point2F(c.x + s * 0.3f, c.y + s * 0.38f), w * 0.85f, w * 0.85f), b);
            break;
        }
        case Kind::mitten: {
            ComPtr<ID2D1PathGeometry> path;
            factory->CreatePathGeometry(&path);
            ComPtr<ID2D1GeometrySink> sink;
            path->Open(&sink);
            const float x0 = c.x - s * 0.14f, x1 = c.x + s * 0.3f, top = c.y - s * 0.45f, cuff = c.y + s * 0.22f;
            sink->BeginFigure(Point2F(x0, cuff), D2D1_FIGURE_BEGIN_HOLLOW);
            sink->AddLine(Point2F(x0, c.y - s * 0.02f));
            // большой палец
            sink->AddLine(Point2F(c.x - s * 0.36f, c.y - s * 0.16f));
            sink->AddArc(D2D1::ArcSegment(Point2F(c.x - s * 0.28f, c.y - s * 0.3f), D2D1::SizeF(s * 0.08f, s * 0.08f), 0,
                                          D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
            sink->AddLine(Point2F(x0, c.y - s * 0.18f));
            sink->AddLine(Point2F(x0, top + s * 0.22f));
            sink->AddArc(D2D1::ArcSegment(Point2F(x1, top + s * 0.22f), D2D1::SizeF(s * 0.22f, s * 0.22f), 0,
                                          D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
            sink->AddLine(Point2F(x1, cuff));
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            sink->Close();
            rt->DrawGeometry(path.Get(), b, w, st);
            rt->DrawRoundedRectangle(
                D2D1::RoundedRect(D2D1::RectF(x0 - s * 0.03f, cuff, x1 + s * 0.03f, c.y + s * 0.42f), w, w), b, w, st);
            break;
        }
        case Kind::shovel: {
            line(rt, c.x + s * 0.4f, c.y - s * 0.4f, c.x - s * 0.02f, c.y + s * 0.02f, b, w, st);
            line(rt, c.x + s * 0.3f, c.y - s * 0.5f, c.x + s * 0.5f, c.y - s * 0.3f, b, w, st);
            ComPtr<ID2D1PathGeometry> path;
            factory->CreatePathGeometry(&path);
            ComPtr<ID2D1GeometrySink> sink;
            path->Open(&sink);
            sink->BeginFigure(Point2F(c.x - s * 0.12f, c.y - s * 0.08f), D2D1_FIGURE_BEGIN_HOLLOW);
            sink->AddLine(Point2F(c.x + s * 0.08f, c.y + s * 0.12f));
            sink->AddLine(Point2F(c.x - s * 0.18f, c.y + s * 0.44f));
            sink->AddLine(Point2F(c.x - s * 0.44f, c.y + s * 0.18f));
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            sink->Close();
            rt->DrawGeometry(path.Get(), b, w, st);
            break;
        }
        case Kind::snowball: {
            rt->DrawEllipse(D2D1::Ellipse(c, s * 0.42f, s * 0.42f), b, w, st);
            for (int i = 0; i < 8; ++i) {
                const float a = static_cast<float>(i * 3.14159265 / 4);
                rt->FillEllipse(D2D1::Ellipse(Point2F(c.x + std::cos(a) * s * 0.2f, c.y + std::sin(a) * s * 0.2f),
                                              w * 0.55f, w * 0.55f),
                                b);
            }
            break;
        }
    }
}

HICON Icons::makeTrayIcon(ID2D1Factory* factory, IWICImagingFactory* wic, int size, bool lightTaskbar, bool dimmed) {
    if (!factory || !wic) return nullptr;
    ComPtr<IWICBitmap> bitmap;
    if (FAILED(wic->CreateBitmap(size, size, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bitmap)))
        return nullptr;
    ComPtr<ID2D1RenderTarget> rt;
    const auto props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    if (FAILED(factory->CreateWicBitmapRenderTarget(bitmap.Get(), props, &rt))) return nullptr;
    ComPtr<ID2D1SolidColorBrush> brush;
    const float tone = lightTaskbar ? 0.1f : 1.0f;
    rt->CreateSolidColorBrush(D2D1::ColorF(tone, tone, tone, dimmed ? 0.45f : 1.0f), &brush);
    rt->BeginDraw();
    rt->Clear(D2D1::ColorF(0, 0, 0, 0));
    const float s = static_cast<float>(size);
    Icons::draw(rt.Get(), factory, Kind::snowflake, D2D1::Point2F(s / 2, s / 2), s * 0.9f, brush.Get(),
                std::max(1.0f, s / 14.0f));
    if (FAILED(rt->EndDraw())) return nullptr;

    // Иконке нужен непремультиплицированный BGRA.
    std::vector<uint8_t> pixels(static_cast<size_t>(size) * size * 4);
    bitmap->CopyPixels(nullptr, size * 4, static_cast<UINT>(pixels.size()), pixels.data());
    for (size_t i = 0; i < pixels.size(); i += 4) {
        const uint8_t a = pixels[i + 3];
        if (a > 0 && a < 255) {
            for (int k = 0; k < 3; ++k) pixels[i + k] = static_cast<uint8_t>(std::min(255, (pixels[i + k] * 255 + a / 2) / a));
        }
    }
    BITMAPV5HEADER header{};
    header.bV5Size = sizeof(header);
    header.bV5Width = size;
    header.bV5Height = -size;
    header.bV5Planes = 1;
    header.bV5BitCount = 32;
    header.bV5Compression = BI_BITFIELDS;
    header.bV5RedMask = 0x00FF0000;
    header.bV5GreenMask = 0x0000FF00;
    header.bV5BlueMask = 0x000000FF;
    header.bV5AlphaMask = 0xFF000000;
    void* bits = nullptr;
    HDC dc = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(dc, reinterpret_cast<BITMAPINFO*>(&header), DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, dc);
    if (!color) return nullptr;
    memcpy(bits, pixels.data(), pixels.size());
    std::vector<uint8_t> maskBits(static_cast<size_t>((size + 15) / 16 * 2) * size, 0);
    HBITMAP mask = CreateBitmap(size, size, 1, 1, maskBits.data());
    ICONINFO info{};
    info.fIcon = TRUE;
    info.hbmColor = color;
    info.hbmMask = mask;
    HICON icon = CreateIconIndirect(&info);
    DeleteObject(color);
    DeleteObject(mask);
    return icon;
}
