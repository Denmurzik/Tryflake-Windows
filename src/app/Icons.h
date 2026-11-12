#pragma once

#include "Common.h"

/// Векторные значки, рисуются Direct2D в любом render target.
namespace Icons {
enum class Kind { snowflake, cloudSnow, windSnow, mitten, shovel, snowball };
/// Освобождает кэш стиля штриха; вызывать до gpu_.destroy() и CoUninitialize.
void releaseCache();
void draw(ID2D1RenderTarget* rt, ID2D1Factory* factory, Kind kind, D2D1_POINT_2F center, float size,
          ID2D1Brush* brush, float stroke);
/// Иконка в области уведомлений: белая/тёмная снежинка, приглушённая на паузе.
HICON makeTrayIcon(ID2D1Factory* factory, IWICImagingFactory* wic, int size, bool lightTaskbar, bool dimmed);
}
