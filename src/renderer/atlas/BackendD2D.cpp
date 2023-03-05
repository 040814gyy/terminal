#include "pch.h"
#include "BackendD2D.h"

TIL_FAST_MATH_BEGIN

// Disable a bunch of warnings which get in the way of writing performant code.
#pragma warning(disable : 26429) // Symbol 'data' is never tested for nullness, it can be marked as not_null (f.23).
#pragma warning(disable : 26446) // Prefer to use gsl::at() instead of unchecked subscript operator (bounds.4).
#pragma warning(disable : 26459) // You called an STL function '...' with a raw pointer parameter at position '...' that may be unsafe [...].
#pragma warning(disable : 26481) // Don't use pointer arithmetic. Use span instead (bounds.1).
#pragma warning(disable : 26482) // Only index into arrays using constant expressions (bounds.2).

using namespace Microsoft::Console::Render::Atlas;

BackendD2D::BackendD2D(wil::com_ptr<ID3D11Device2> device, wil::com_ptr<ID3D11DeviceContext2> deviceContext) :
    _device{ std::move(device) },
    _deviceContext{ std::move(deviceContext) }
{
}

void BackendD2D::Render(RenderingPayload& p)
{
    if (_generation != p.s.generation())
    {
        _handleSettingsUpdate(p);
    }

    _renderTarget->BeginDraw();
    _drawBackground(p);
    _drawText(p);
    _drawGridlines(p);
    _drawCursor(p);
    _drawSelection(p);
    THROW_IF_FAILED(_renderTarget->EndDraw());

    _swapChainManager.Present(p);
}

bool BackendD2D::RequiresContinuousRedraw() noexcept
{
    return false;
}

void BackendD2D::WaitUntilCanRender() noexcept
{
    _swapChainManager.WaitUntilCanRender();
}

void BackendD2D::_handleSettingsUpdate(const RenderingPayload& p)
{
    _swapChainManager.UpdateSwapChainSettings(
        p,
        _device.get(),
        [this]() {
            _renderTarget.reset();
            _renderTarget4.reset();
            _deviceContext->ClearState();
            _deviceContext->Flush();
        },
        [this]() {
            _renderTarget.reset();
            _renderTarget4.reset();
            _deviceContext->ClearState();
        });

    if (!_renderTarget)
    {
        {
            const auto surface = _swapChainManager.GetBuffer().query<IDXGISurface>();

            const D2D1_RENDER_TARGET_PROPERTIES props{
                .type = D2D1_RENDER_TARGET_TYPE_DEFAULT,
                .pixelFormat = { DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED },
                .dpiX = static_cast<f32>(p.s->font->dpi),
                .dpiY = static_cast<f32>(p.s->font->dpi),
            };
            wil::com_ptr<ID2D1RenderTarget> renderTarget;
            THROW_IF_FAILED(p.d2dFactory->CreateDxgiSurfaceRenderTarget(surface.get(), &props, renderTarget.addressof()));
            _renderTarget = renderTarget.query<ID2D1DeviceContext>();
            _renderTarget4 = renderTarget.try_query<ID2D1DeviceContext4>();
            _renderTarget->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
        }
        {
            static constexpr D2D1_COLOR_F color{ 1, 1, 1, 1 };
            THROW_IF_FAILED(_renderTarget->CreateSolidColorBrush(&color, nullptr, _brush.put()));
            _brushColor = 0xffffffff;
        }
    }

    if (!_dottedStrokeStyle)
    {
        static constexpr D2D1_STROKE_STYLE_PROPERTIES props{ .dashStyle = D2D1_DASH_STYLE_CUSTOM };
        static constexpr FLOAT dashes[2]{ 1, 2 };
        THROW_IF_FAILED(p.d2dFactory->CreateStrokeStyle(&props, &dashes[0], 2, _dottedStrokeStyle.addressof()));
    }

    const auto fontChanged = _fontGeneration != p.s->font.generation();
    const auto cellCountChanged = _cellCount != p.s->cellCount;

    if (fontChanged)
    {
        const auto dpi = static_cast<f32>(p.s->font->dpi);
        _renderTarget->SetDpi(dpi, dpi);
        _renderTarget->SetTextAntialiasMode(static_cast<D2D1_TEXT_ANTIALIAS_MODE>(p.s->font->antialiasingMode));
    }

    if (fontChanged || cellCountChanged)
    {
        const D2D1_BITMAP_PROPERTIES props{
            .pixelFormat = { DXGI_FORMAT_R8G8B8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED },
            .dpiX = static_cast<f32>(p.s->font->dpi),
            .dpiY = static_cast<f32>(p.s->font->dpi),
        };
        const D2D1_SIZE_U size{ p.s->cellCount.x, p.s->cellCount.y };
        const D2D1_MATRIX_3X2_F transform{
            ._11 = static_cast<f32>(p.s->font->cellSize.x),
            ._22 = static_cast<f32>(p.s->font->cellSize.y),
        };
        THROW_IF_FAILED(_renderTarget->CreateBitmap(size, nullptr, 0, &props, _backgroundBitmap.put()));
        THROW_IF_FAILED(_renderTarget->CreateBitmapBrush(_backgroundBitmap.get(), _backgroundBrush.put()));
        _backgroundBrush->SetInterpolationMode(D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
        _backgroundBrush->SetExtendModeX(D2D1_EXTEND_MODE_MIRROR);
        _backgroundBrush->SetExtendModeY(D2D1_EXTEND_MODE_MIRROR);
        _backgroundBrush->SetTransform(&transform);
    }

    _generation = p.s.generation();
    _fontGeneration = p.s->font.generation();
    _cellCount = p.s->cellCount;
}

void BackendD2D::_drawBackground(const RenderingPayload& p)
{
    // If the terminal was 120x30 cells and 1200x600 pixels large, this would draw the
    // background by upscaling a 120x30 pixel bitmap to fill the entire render target.
    const D2D1_RECT_F rect{ 0, 0, p.s->targetSize.x * p.d.font.dipPerPixel, p.s->targetSize.y * p.d.font.dipPerPixel };
    _renderTarget->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_COPY);
    _backgroundBitmap->CopyFromMemory(nullptr, p.backgroundBitmap.data(), p.s->cellCount.x * 4);
    _renderTarget->FillRectangle(&rect, _backgroundBrush.get());
    _renderTarget->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_SOURCE_OVER);
}

void BackendD2D::_drawText(RenderingPayload& p)
{
    // It is possible to create a "_foregroundBrush" similar to how the `_backgroundBrush` is created and
    // use that as the brush for text rendering below. That way we wouldn't have to search `row.colors` for color
    // changes and could draw entire lines of text in a single call. Unfortunately Direct2D is not particularly
    // smart if you do this and chooses to draw the given text into a way too small offscreen texture first and
    // then blends it on the screen with the given bitmap brush. While this roughly doubles the performance
    // when drawing lots of colors, the extra latency drops performance by >10x when drawing fewer colors.
    // Since fewer colors are more common, I've chosen to go with regular solid-color brushes.
    u16 y = 0;
    for (auto& row : p.rows)
    {
        f32 baselineX = 0.0f;

        for (const auto& m : row.mappings)
        {
            const auto colorsBegin = row.colors.begin();
            auto it = colorsBegin + m.glyphsFrom;
            const auto end = colorsBegin + m.glyphsTo;

            do
            {
                const auto beg = it;
                const auto off = it - colorsBegin;
                const auto fg = *it;

                while (++it != end && *it == fg)
                {
                }

                const auto count = it - beg;
                const auto brush = _brushWithColor(fg);
                const auto baselineY = p.d.font.cellSizeDIP.y * y + p.s->font->baselineInDIP;
                const DWRITE_GLYPH_RUN glyphRun{
                    .fontFace = m.fontFace.get(),
                    .fontEmSize = m.fontEmSize,
                    .glyphCount = static_cast<UINT32>(count),
                    .glyphIndices = &row.glyphIndices[off],
                    .glyphAdvances = &row.glyphAdvances[off],
                    .glyphOffsets = &row.glyphOffsets[off],
                };

                DrawGlyphRun(_renderTarget.get(), _renderTarget4.get(), p.dwriteFactory4.get(), { baselineX, baselineY }, &glyphRun, brush);

                const auto blackBox = GetGlyphRunBlackBox(glyphRun, baselineX, baselineY);
                // Add a 1px padding to avoid inaccuracies with the blackbox measurement.
                // It's only an estimate based on the design size after all.
                row.top = std::min(row.top, static_cast<i32>(ceilf(blackBox.top) + 1.5f));
                row.bottom = std::max(row.bottom, static_cast<i32>(floorf(blackBox.bottom) + 1.5f));

                for (UINT32 i = 0; i < glyphRun.glyphCount; ++i)
                {
                    baselineX += glyphRun.glyphAdvances[i];
                }
            } while (it != end);
        }

        y++;
    }
}

void BackendD2D::_drawGridlines(const RenderingPayload& p)
{
    u16 y = 0;
    for (const auto& row : p.rows)
    {
        const auto top = p.d.font.cellSizeDIP.y * y;
        const auto bottom = p.d.font.cellSizeDIP.y * (y + 1);

        for (const auto& r : row.gridLineRanges)
        {
            // AtlasEngine.cpp shouldn't add any gridlines if they don't do anything.
            assert(r.lines.any());

            D2D1_RECT_F rect{ r.from * p.d.font.cellSizeDIP.x, top, r.to * p.d.font.cellSizeDIP.x, bottom };

            if (r.lines.test(GridLines::Left))
            {
                for (auto i = r.from; i < r.to; ++i)
                {
                    rect.left = i * p.d.font.cellSizeDIP.x;
                    rect.right = rect.left + p.s->font->thinLineWidth * p.d.font.dipPerPixel;
                    _fillRectangle(rect, r.color);
                }
            }
            if (r.lines.test(GridLines::Top))
            {
                rect.bottom = rect.top + p.s->font->thinLineWidth * p.d.font.dipPerPixel;
                _fillRectangle(rect, r.color);
            }
            if (r.lines.test(GridLines::Right))
            {
                for (auto i = r.to; i > r.from; --i)
                {
                    rect.right = i * p.d.font.cellSizeDIP.x;
                    rect.left = rect.right - p.s->font->thinLineWidth * p.d.font.dipPerPixel;
                    _fillRectangle(rect, r.color);
                }
            }
            if (r.lines.test(GridLines::Bottom))
            {
                rect.top = rect.bottom - p.s->font->thinLineWidth * p.d.font.dipPerPixel;
                _fillRectangle(rect, r.color);
            }
            if (r.lines.test(GridLines::Underline))
            {
                rect.top += p.s->font->underlinePos * p.d.font.dipPerPixel;
                rect.bottom = rect.top + p.s->font->underlineWidth * p.d.font.dipPerPixel;
                _fillRectangle(rect, r.color);
            }
            if (r.lines.test(GridLines::HyperlinkUnderline))
            {
                const auto w = p.s->font->underlineWidth * p.d.font.dipPerPixel;
                const auto centerY = rect.top + p.s->font->underlinePos * p.d.font.dipPerPixel + w * 0.5f;
                const auto brush = _brushWithColor(r.color);
                const D2D1_POINT_2F point0{ rect.left, centerY };
                const D2D1_POINT_2F point1{ rect.right, centerY };
                _renderTarget->DrawLine(point0, point1, brush, w, _dottedStrokeStyle.get());
            }
            if (r.lines.test(GridLines::DoubleUnderline))
            {
                rect.top = top + p.s->font->doubleUnderlinePos.x * p.d.font.dipPerPixel;
                rect.bottom = rect.top + p.s->font->thinLineWidth * p.d.font.dipPerPixel;
                _fillRectangle(rect, r.color);

                rect.top = top + p.s->font->doubleUnderlinePos.y * p.d.font.dipPerPixel;
                rect.bottom = rect.top + p.s->font->thinLineWidth * p.d.font.dipPerPixel;
                _fillRectangle(rect, r.color);
            }
            if (r.lines.test(GridLines::Strikethrough))
            {
                rect.top = top + p.s->font->strikethroughPos * p.d.font.dipPerPixel;
                rect.bottom = rect.top + p.s->font->strikethroughWidth * p.d.font.dipPerPixel;
                _fillRectangle(rect, r.color);
            }
        }

        y++;
    }
}

void BackendD2D::_drawCursor(const RenderingPayload& p)
{
    if (!p.cursorRect)
    {
        return;
    }

    D2D1_RECT_F rect{
        p.d.font.cellSizeDIP.x * p.cursorRect.left,
        p.d.font.cellSizeDIP.y * p.cursorRect.top,
        p.d.font.cellSizeDIP.x * p.cursorRect.right,
        p.d.font.cellSizeDIP.y * p.cursorRect.bottom,
    };

    switch (static_cast<CursorType>(p.s->cursor->cursorType))
    {
    case CursorType::Legacy:
        rect.top = rect.bottom - (rect.bottom - rect.top) * static_cast<float>(p.s->cursor->heightPercentage) / 100.0f;
        _fillRectangle(rect, p.s->cursor->cursorColor);
        break;
    case CursorType::VerticalBar:
        rect.right = rect.left + p.s->font->thinLineWidth * p.d.font.dipPerPixel;
        _fillRectangle(rect, p.s->cursor->cursorColor);
        break;
    case CursorType::Underscore:
        rect.top += p.s->font->underlinePos * p.d.font.dipPerPixel;
        rect.bottom = rect.top + p.s->font->underlineWidth * p.d.font.dipPerPixel;
        _fillRectangle(rect, p.s->cursor->cursorColor);
        break;
    case CursorType::EmptyBox:
    {
        const auto brush = _brushWithColor(p.s->cursor->cursorColor);
        const auto w = p.s->font->thinLineWidth * p.d.font.dipPerPixel;
        const auto wh = w / 2.0f;
        rect.left += wh;
        rect.top += wh;
        rect.right += wh;
        rect.bottom += wh;
        _renderTarget->DrawRectangle(&rect, brush, w, nullptr);
        break;
    }
    case CursorType::FullBox:
        _fillRectangle(rect, p.s->cursor->cursorColor);
        break;
    case CursorType::DoubleUnderscore:
    {
        auto rect2 = rect;
        rect2.top = rect.top + p.s->font->doubleUnderlinePos.x * p.d.font.dipPerPixel;
        rect2.bottom = rect2.top + p.s->font->thinLineWidth * p.d.font.dipPerPixel;
        _fillRectangle(rect2, p.s->cursor->cursorColor);
        rect.top = rect.top + p.s->font->doubleUnderlinePos.y * p.d.font.dipPerPixel;
        rect.bottom = rect.top + p.s->font->thinLineWidth * p.d.font.dipPerPixel;
        _fillRectangle(rect, p.s->cursor->cursorColor);
        break;
    }
    default:
        break;
    }
}

void BackendD2D::_drawSelection(const RenderingPayload& p)
{
    u16 y = 0;
    for (const auto& row : p.rows)
    {
        if (row.selectionTo > row.selectionFrom)
        {
            const D2D1_RECT_F rect{
                p.d.font.cellSizeDIP.x * row.selectionFrom,
                p.d.font.cellSizeDIP.y * y,
                p.d.font.cellSizeDIP.x * row.selectionTo,
                p.d.font.cellSizeDIP.y * (y + 1),
            };
            _fillRectangle(rect, p.s->misc->selectionColor);
        }

        y++;
    }
}

ID2D1Brush* BackendD2D::_brushWithColor(u32 color)
{
    if (_brushColor != color)
    {
        const auto d2dColor = colorFromU32(color);
        THROW_IF_FAILED(_renderTarget->CreateSolidColorBrush(&d2dColor, nullptr, _brush.put()));
        _brushColor = color;
    }
    return _brush.get();
}

void BackendD2D::_fillRectangle(const D2D1_RECT_F& rect, u32 color)
{
    const auto brush = _brushWithColor(color);
    _renderTarget->FillRectangle(&rect, brush);
}

TIL_FAST_MATH_END
