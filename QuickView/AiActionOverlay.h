#pragma once
// ============================================================================
// AiActionOverlay.h - Win11 Geek Glass AI Actions HUD & Modal Controller
// ============================================================================

#include "pch.h"
#include <vector>
#include <string>
#include <d2d1_2.h>
#include <dwrite.h>
#include <wrl/client.h>
#include "GeekGlass.h"
#include "AiActionTypes.h"

namespace QuickView::UI {

using Microsoft::WRL::ComPtr;

class AiActionOverlay {
public:
    static AiActionOverlay& Instance();

    void Init(ID2D1DeviceContext* dc, HWND hwnd);
    void Render(ID2D1DeviceContext* dc, float winW, float winH);
    void SetUIScale(float scale) { m_uiScale = scale; }

    void Show();
    void Hide();
    void Toggle();
    bool IsVisible() const { return m_visible; }

    // Input handlers (returns true if intercepted in modal state)
    bool OnKeyDown(WPARAM key);
    bool OnMouseMove(float x, float y);
    bool OnLButtonDown(float x, float y);
    bool OnMouseWheel(float delta);
    int GetHoverIndex() const { return m_hoverIndex; }

    // Set background command list for Geek Glass blur effect
    void SetGeekGlassData(ID2D1CommandList* list, const D2D1_MATRIX_3X2_F& transform) {
        m_bgCmdList = list;
        m_bgTransform = transform;
    }

private:
    AiActionOverlay();
    ~AiActionOverlay() = default;

    void CreateDeviceResources(ID2D1DeviceContext* dc);
    void TriggerAction(size_t index);

    HWND m_hwnd = nullptr;
    bool m_visible = false;
    float m_uiScale = 1.0f;
    int m_selectedIndex = 0;
    int m_hoverIndex = -1;

    D2D1_RECT_F m_hudRect = {};
    std::vector<D2D1_RECT_F> m_itemRects;

    // D2D Resources
    ComPtr<ID2D1SolidColorBrush> m_brushBg;
    ComPtr<ID2D1SolidColorBrush> m_brushCard;
    ComPtr<ID2D1SolidColorBrush> m_brushCardHover;
    ComPtr<ID2D1SolidColorBrush> m_brushCardSelected;
    ComPtr<ID2D1SolidColorBrush> m_brushBorder;
    ComPtr<ID2D1SolidColorBrush> m_brushAccent;
    ComPtr<ID2D1SolidColorBrush> m_brushText;
    ComPtr<ID2D1SolidColorBrush> m_brushTextDim;
    ComPtr<ID2D1SolidColorBrush> m_brushKeyBadge;

    ComPtr<IDWriteTextFormat> m_fontTitle;
    ComPtr<IDWriteTextFormat> m_fontItem;
    ComPtr<IDWriteTextFormat> m_fontDetail;
    ComPtr<IDWriteTextFormat> m_fontBadge;

    ComPtr<ID2D1CommandList> m_bgCmdList;
    D2D1_MATRIX_3X2_F m_bgTransform = D2D1::Matrix3x2F::Identity();
    QuickView::UI::GeekGlass::GeekGlassEngine m_geekGlass;
};

} // namespace QuickView::UI
