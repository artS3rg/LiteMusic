#pragma once

#include <commctrl.h>
#include <windowsx.h>
#include <algorithm>
#include <initializer_list>
#include <string_view>

namespace ui {
constexpr COLORREF background = RGB(18, 22, 29);
constexpr COLORREF header = RGB(21, 26, 35);
constexpr COLORREF panel = RGB(25, 32, 42);
constexpr COLORREF field = RGB(20, 26, 35);
constexpr COLORREF button = RGB(33, 43, 55);
constexpr COLORREF hover = RGB(41, 54, 68);
constexpr COLORREF active = RGB(31, 57, 59);
constexpr COLORREF selected = RGB(36, 49, 63);
constexpr COLORREF line = RGB(44, 55, 70);
constexpr COLORREF rail = RGB(55, 69, 86);
constexpr COLORREF ink = RGB(232, 239, 247);
constexpr COLORREF muted = RGB(157, 173, 194);
constexpr COLORREF dim = RGB(106, 124, 148);
constexpr COLORREF mint = RGB(119, 234, 200);
constexpr COLORREF mint_hover = RGB(159, 248, 222);
constexpr COLORREF on_mint = RGB(15, 36, 32);

inline int px(int size, UINT dpi) { return MulDiv(size, dpi, 96); }

inline void fill(HDC dc, RECT r, COLORREF color) {
    SetDCBrushColor(dc, color);
    FillRect(dc, &r, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
}

inline void box(HDC dc, RECT r, int radius, COLORREF color, COLORREF border) {
    auto old_pen = SelectObject(dc, GetStockObject(border == color ? NULL_PEN : DC_PEN));
    auto old_brush = SelectObject(dc, GetStockObject(DC_BRUSH));
    SetDCBrushColor(dc, color);
    SetDCPenColor(dc, border);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, radius, radius);
    SelectObject(dc, old_brush);
    SelectObject(dc, old_pen);
}

inline void text(HDC dc, RECT r, std::wstring_view value, COLORREF color, HFONT font,
                 UINT flags = DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS) {
    if (value.empty()) return;
    auto old_font = SelectObject(dc, font);
    SetTextColor(dc, color);
    SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, value.data(), static_cast<int>(value.size()), &r, flags | DT_NOPREFIX);
    SelectObject(dc, old_font);
}

enum class Glyph { play, pause, previous, next, heart, liked, search, folder, wave, volume, mini, expand, pin };

inline void glyph(HDC dc, int x, int y, UINT dpi, Glyph symbol, COLORREF color) {
    auto line_pen = CreatePen(PS_SOLID, std::max(1, px(1, dpi)), color);
    auto old_pen = SelectObject(dc, line_pen);
    auto old_brush = SelectObject(dc, GetStockObject(DC_BRUSH));
    SetDCBrushColor(dc, color);
    auto p = [=](int dx, int dy) { return POINT{x + px(dx, dpi), y + px(dy, dpi)}; };
    auto path = [&](std::initializer_list<POINT> points, bool closed = false) {
        if (closed) Polygon(dc, points.begin(), static_cast<int>(points.size()));
        else Polyline(dc, points.begin(), static_cast<int>(points.size()));
    };
    auto chip = [&](int x1, int y1, int x2, int y2) {
        auto a = p(x1, y1), b = p(x2, y2);
        fill(dc, {a.x, a.y, b.x, b.y}, color);
    };
    switch (symbol) {
    case Glyph::play: path({p(-4,-6), p(6,0), p(-4,6)}, true); break;
    case Glyph::pause: chip(-5,-6,-2,6); chip(2,-6,5,6); break;
    case Glyph::previous: path({p(5,-5), p(-4,0), p(5,5)}, true); chip(-7,-6,-5,6); break;
    case Glyph::next: path({p(-5,-5), p(4,0), p(-5,5)}, true); chip(5,-6,7,6); break;
    case Glyph::heart:
    case Glyph::liked: {
        auto brush = SelectObject(dc, GetStockObject(symbol == Glyph::liked ? DC_BRUSH : NULL_BRUSH));
        POINT curve[] = {p(0,6), p(-3,3), p(-7,1), p(-7,-2), p(-7,-6), p(-2,-7), p(0,-3),
                         p(2,-7), p(7,-6), p(7,-2), p(7,1), p(3,3), p(0,6)};
        BeginPath(dc);
        PolyBezier(dc, curve, 13);
        CloseFigure(dc);
        EndPath(dc);
        if (symbol == Glyph::liked) StrokeAndFillPath(dc);
        else StrokePath(dc);
        SelectObject(dc, brush);
        break;
    }
    case Glyph::search: {
        auto brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
        auto a = p(-6,-6), b = p(4,4);
        Ellipse(dc, a.x, a.y, b.x, b.y);
        path({p(3,3),p(7,7)});
        SelectObject(dc, brush);
        break;
    }
    case Glyph::folder:
        path({p(-7,-4),p(-2,-4),p(0,-2),p(7,-2),p(7,5),p(-7,5),p(-7,-4)});
        break;
    case Glyph::wave:
        chip(-6,-2,-5,3); chip(-3,-5,-2,6); chip(0,-8,1,9); chip(3,-5,4,6); chip(6,-2,7,3);
        break;
    case Glyph::volume:
        path({p(-7,-2),p(-4,-2),p(0,-5),p(0,5),p(-4,2),p(-7,2)}, true);
        path({p(2,-2),p(3,-1),p(3,1),p(2,2)});
        path({p(4,-4),p(6,-2),p(6,2),p(4,4)});
        break;
    case Glyph::mini:
        path({p(-6,-4),p(6,-4),p(6,4),p(-6,4),p(-6,-4)});
        chip(-2,0,5,3);
        break;
    case Glyph::expand:
        path({p(-5,1),p(-5,5),p(-1,5)});
        path({p(1,-5),p(5,-5),p(5,-1)});
        path({p(-4,4),p(4,-4)});
        break;
    case Glyph::pin:
        path({p(-3,-6),p(3,-6),p(2,-2),p(5,2),p(-5,2),p(-2,-2),p(-3,-6)});
        path({p(0,2),p(0,8)});
        break;
    }
    SelectObject(dc, old_brush);
    SelectObject(dc, old_pen);
    DeleteObject(line_pen);
}

inline LRESULT CALLBACK button_proc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (message == WM_MOUSEMOVE && !GetPropW(hwnd, L"LiteMusic.Hover")) {
        TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, hwnd, 0};
        if (TrackMouseEvent(&track)) {
            SetPropW(hwnd, L"LiteMusic.Hover", reinterpret_cast<HANDLE>(1));
            InvalidateRect(hwnd, nullptr, FALSE);
        }
    } else if (message == WM_MOUSELEAVE || message == WM_NCDESTROY) {
        RemovePropW(hwnd, L"LiteMusic.Hover");
        if (message == WM_MOUSELEAVE) InvalidateRect(hwnd, nullptr, FALSE);
    }
    return DefSubclassProc(hwnd, message, wp, lp);
}

inline RECT slider_rail(HWND hwnd) {
    RECT channel{}, thumb{};
    SendMessageW(hwnd, TBM_GETCHANNELRECT, 0, reinterpret_cast<LPARAM>(&channel));
    SendMessageW(hwnd, TBM_GETTHUMBRECT, 0, reinterpret_cast<LPARAM>(&thumb));
    auto half = (thumb.right - thumb.left) / 2;
    channel.left += half;
    channel.right -= half;
    return channel;
}

// Let Win32 handle thumb dragging and the keyboard; channel clicks jump directly.
inline LRESULT CALLBACK slider_proc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (message == WM_ERASEBKGND) return 1;
    if (message == WM_PAINT) {
        PAINTSTRUCT ps{};
        auto dc = BeginPaint(hwnd, &ps);
        RECT client{}, thumb{};
        GetClientRect(hwnd, &client);
        auto channel = slider_rail(hwnd);
        SendMessageW(hwnd, TBM_GETTHUMBRECT, 0, reinterpret_cast<LPARAM>(&thumb));
        auto dpi = GetDpiForWindow(hwnd);
        auto y = client.bottom / 2;
        auto height = std::max(2, px(4, dpi));
        auto x = std::clamp((thumb.left + thumb.right) / 2, channel.left, std::max(channel.left, channel.right));
        auto enabled = IsWindowEnabled(hwnd);
        auto focus = GetFocus() == hwnd;
        fill(dc, client, panel);
        auto rail_rect = RECT{channel.left, y-height/2, channel.right, y+(height+1)/2};
        box(dc, rail_rect, height, enabled ? rail : line, enabled ? rail : line);
        if (enabled && x > channel.left) {
            rail_rect.right = x;
            box(dc, rail_rect, height, mint, mint);
        }
        auto radius = px(4, dpi);
        auto color = enabled ? (focus ? mint_hover : ink) : dim;
        box(dc, {x-radius, y-radius, x+radius+1, y+radius+1}, 2*radius+1, color, color);
        if (focus) {
            auto edge = px(2, dpi);
            HPEN pen = CreatePen(PS_SOLID, 1, mint);
            auto old_pen = SelectObject(dc, pen);
            auto old_brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
            Ellipse(dc, x-radius-edge, y-radius-edge, x+radius+edge+1, y+radius+edge+1);
            SelectObject(dc, old_brush);
            SelectObject(dc, old_pen);
            DeleteObject(pen);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    if (message == WM_LBUTTONDOWN) {
        RECT thumb{};
        SendMessageW(hwnd, TBM_GETTHUMBRECT, 0, reinterpret_cast<LPARAM>(&thumb));
        const auto x = GET_X_LPARAM(lp);
        if (x < thumb.left || x >= thumb.right) {
            auto channel = slider_rail(hwnd);
            if (channel.right > channel.left) {
                SetFocus(hwnd);
                auto range = static_cast<int>(SendMessageW(hwnd, TBM_GETRANGEMAX, 0, 0));
                auto position = MulDiv(std::clamp(x - channel.left, 0L, channel.right - channel.left),
                                       range, channel.right - channel.left);
                SendMessageW(hwnd, TBM_SETPOS, TRUE, position);
                SendMessageW(GetParent(hwnd), WM_HSCROLL, MAKEWPARAM(TB_THUMBPOSITION, position),
                             reinterpret_cast<LPARAM>(hwnd));
                return 0;
            }
        }
    }
    auto handled = DefSubclassProc(hwnd, message, wp, lp);
    if (message == WM_SETFOCUS || message == WM_KILLFOCUS || message == WM_ENABLE ||
        (message == TBM_SETPOS && wp) || message == WM_LBUTTONDOWN || message == WM_LBUTTONUP ||
        (message == WM_MOUSEMOVE && (wp & MK_LBUTTON)) || message == WM_KEYDOWN || message == WM_MOUSEWHEEL) {
        InvalidateRect(hwnd, nullptr, FALSE);
    }
    return handled;
}
}
