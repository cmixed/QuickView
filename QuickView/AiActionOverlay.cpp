#include "pch.h"
#include "AiActionOverlay.h"
#include "AiActionManager.h"
#include "AppContext.h"
#include "CompareController.h"
#include "ThemeSystem.h"
#include "OSDState.h"
#include "EditState.h"

extern OSDState g_osd;
extern HWND g_mainHwnd;
extern void RequestRepaint(QuickView::PaintLayer layer);

namespace QuickView::UI {

AiActionOverlay& AiActionOverlay::Instance() {
    static AiActionOverlay instance;
    return instance;
}

AiActionOverlay::AiActionOverlay() {
}

void AiActionOverlay::Init(ID2D1DeviceContext* dc, HWND hwnd) {
    m_hwnd = hwnd;
    CreateDeviceResources(dc);
}

void AiActionOverlay::CreateDeviceResources(ID2D1DeviceContext* dc) {
    if (!dc) return;

    bool isLight = IsLightThemeActive();
    D2D1_COLOR_F bgClr = isLight ? D2D1::ColorF(0.96f, 0.96f, 0.98f, 0.98f) : D2D1::ColorF(0.12f, 0.12f, 0.14f, 0.98f);
    D2D1_COLOR_F cardClr = isLight ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.8f) : D2D1::ColorF(0.18f, 0.18f, 0.22f, 0.85f);
    D2D1_COLOR_F hoverClr = isLight ? D2D1::ColorF(0.0f, 0.45f, 0.9f, 0.12f) : D2D1::ColorF(0.0f, 0.5f, 1.0f, 0.25f);
    D2D1_COLOR_F selectClr = isLight ? D2D1::ColorF(0.0f, 0.45f, 0.9f, 0.25f) : D2D1::ColorF(0.0f, 0.5f, 1.0f, 0.40f);
    D2D1_COLOR_F borderClr = isLight ? D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.15f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.18f);
    D2D1_COLOR_F accentClr = isLight ? D2D1::ColorF(0.0f, 0.45f, 0.95f, 1.0f) : D2D1::ColorF(0.2f, 0.65f, 1.0f, 1.0f);
    D2D1_COLOR_F textClr = isLight ? D2D1::ColorF(0.1f, 0.1f, 0.12f, 1.0f) : D2D1::ColorF(0.95f, 0.95f, 0.98f, 1.0f);
    D2D1_COLOR_F textDimClr = isLight ? D2D1::ColorF(0.45f, 0.45f, 0.5f, 1.0f) : D2D1::ColorF(0.6f, 0.6f, 0.65f, 1.0f);
    D2D1_COLOR_F badgeClr = isLight ? D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.08f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.10f);

    if (!m_brushBg) {
        dc->CreateSolidColorBrush(bgClr, &m_brushBg);
        dc->CreateSolidColorBrush(cardClr, &m_brushCard);
        dc->CreateSolidColorBrush(hoverClr, &m_brushCardHover);
        dc->CreateSolidColorBrush(selectClr, &m_brushCardSelected);
        dc->CreateSolidColorBrush(borderClr, &m_brushBorder);
        dc->CreateSolidColorBrush(accentClr, &m_brushAccent);
        dc->CreateSolidColorBrush(textClr, &m_brushText);
        dc->CreateSolidColorBrush(textDimClr, &m_brushTextDim);
        dc->CreateSolidColorBrush(badgeClr, &m_brushKeyBadge);
    } else {
        m_brushBg->SetColor(bgClr);
        m_brushCard->SetColor(cardClr);
        m_brushCardHover->SetColor(hoverClr);
        m_brushCardSelected->SetColor(selectClr);
        m_brushBorder->SetColor(borderClr);
        m_brushAccent->SetColor(accentClr);
        m_brushText->SetColor(textClr);
        m_brushTextDim->SetColor(textDimClr);
        m_brushKeyBadge->SetColor(badgeClr);
    }

    ComPtr<IDWriteFactory> dwrite;
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(dwrite.GetAddressOf()));
    if (dwrite) {
        dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 14.0f * m_uiScale, L"", &m_fontTitle);
        dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 13.0f * m_uiScale, L"", &m_fontItem);
        dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 11.5f * m_uiScale, L"", &m_fontDetail);
        dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 11.5f * m_uiScale, L"", &m_fontBadge);
        if (m_fontBadge) {
            m_fontBadge->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            m_fontBadge->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
    }
}

void AiActionOverlay::Show() {
    m_visible = true;
    m_hoverIndex = -1;
    if (!m_hwnd && g_mainHwnd) m_hwnd = g_mainHwnd;

    // Focus on last used action if available
    const auto& actions = AI::AiActionManager::Instance().GetActions();
    const std::string& lastId = AI::AiActionManager::Instance().GetLastActionId();
    m_selectedIndex = 0;
    for (size_t i = 0; i < actions.size(); ++i) {
        if (actions[i].id == lastId) {
            m_selectedIndex = static_cast<int>(i);
            break;
        }
    }

    RequestRepaint(QuickView::PaintLayer::Static);
    if (m_hwnd) InvalidateRect(m_hwnd, nullptr, FALSE);
}

void AiActionOverlay::Hide() {
    if (!m_visible) return;
    m_visible = false;
    if (!m_hwnd && g_mainHwnd) m_hwnd = g_mainHwnd;
    RequestRepaint(QuickView::PaintLayer::Static);
    if (m_hwnd) InvalidateRect(m_hwnd, nullptr, FALSE);
}

void AiActionOverlay::Toggle() {
    if (m_visible) Hide();
    else Show();
}

void AiActionOverlay::TriggerAction(size_t index) {
    const auto& actions = AI::AiActionManager::Instance().GetActions();
    if (index >= actions.size()) return;

    Hide();

    const auto& act = actions[index];
    AI::AiActionManager::Instance().SetLastActionId(act.id);

    wchar_t msg[128] = { 0 };
    swprintf_s(msg, L"AI 正在处理: %s (按 Esc 取消)...", act.name.c_str());
    g_osd.Show(m_hwnd, msg, false, false, D2D1::ColorF(D2D1::ColorF::LightSkyBlue), OSDPosition::Bottom, 5000);

    AI::AiActionManager::Instance().ExecuteAction(act, m_hwnd, [act](const AI::ExecutionResult& res) {
        if (!res.success) {
            std::wstring errText = res.errorMessage;
            if (errText.rfind(L"AI 执行失败", 0) == std::wstring::npos) {
                errText = L"AI 执行失败: " + errText;
            }
            g_osd.Show(g_mainHwnd, errText.c_str(), false, false, D2D1::ColorF(D2D1::ColorF::OrangeRed), OSDPosition::Bottom, 6000);
            return;
        }

        if (!res.resultImageData.empty()) {
            // Execution succeeded with Image: Show notification and enter Compare Mode
            g_osd.Show(g_mainHwnd, L"AI 生成完成！进入帘幕对比模式", false, false, D2D1::ColorF(D2D1::ColorF::LightGreen), OSDPosition::Bottom, 3000);

            if (AppContext::GetInstance().CompareCtrl && g_mainHwnd) {
                // Enter SR/AI Compare Mode (Wipe View: Left=Original, Right=AI Result)
                AppContext::GetInstance().CompareCtrl->EnterSrCompareMode(g_mainHwnd);
            }
            if (g_mainHwnd) InvalidateRect(g_mainHwnd, nullptr, FALSE);
        } else if (!res.textContent.empty()) {
            // Execution succeeded with Text Content
            std::wstring displayMsg = act.name + L": " + res.textContent;
            g_osd.Show(g_mainHwnd, displayMsg.c_str(), false, false, D2D1::ColorF(D2D1::ColorF::LightGreen), OSDPosition::Bottom, 8000);
        }
    });
}

bool AiActionOverlay::OnKeyDown(WPARAM key) {
    if (!m_visible) return false;

    const auto& actions = AI::AiActionManager::Instance().GetActions();
    int count = static_cast<int>(actions.size());

    // 1. Esc -> Close
    if (key == VK_ESCAPE) {
        Hide();
        return true;
    }

    // 2. Direct Numeric Keys 1..9 -> Trigger instantly
    if (key >= '1' && key <= '9') {
        int idx = static_cast<int>(key - '1');
        if (idx < count) {
            TriggerAction(idx);
            return true;
        }
    }

    // 3. Arrow Keys / Rotation Cycle
    if (key == VK_UP) {
        if (count > 0) {
            m_selectedIndex = (m_selectedIndex - 1 + count) % count;
            RequestRepaint(QuickView::PaintLayer::Static);
            if (m_hwnd) InvalidateRect(m_hwnd, nullptr, FALSE);
        }
        return true;
    }
    if (key == VK_DOWN) {
        if (count > 0) {
            m_selectedIndex = (m_selectedIndex + 1) % count;
            RequestRepaint(QuickView::PaintLayer::Static);
            if (m_hwnd) InvalidateRect(m_hwnd, nullptr, FALSE);
        }
        return true;
    }

    // 4. Enter -> Trigger Selected
    if (key == VK_RETURN) {
        if (m_selectedIndex >= 0 && m_selectedIndex < count) {
            TriggerAction(m_selectedIndex);
            return true;
        }
    }

    return true; // Consume all other keys while modal
}

bool AiActionOverlay::OnMouseMove(float x, float y) {
    if (!m_visible) return false;

    int prevHover = m_hoverIndex;
    m_hoverIndex = -1;

    for (size_t i = 0; i < m_itemRects.size(); ++i) {
        const auto& r = m_itemRects[i];
        if (x >= r.left && x <= r.right && y >= r.top && y <= r.bottom) {
            m_hoverIndex = static_cast<int>(i);
            break;
        }
    }

    if (m_hoverIndex != prevHover) {
        RequestRepaint(QuickView::PaintLayer::Static);
        if (m_hwnd) InvalidateRect(m_hwnd, nullptr, FALSE);
    }
    return true;
}

bool AiActionOverlay::OnLButtonDown(float x, float y) {
    if (!m_visible) return false;

    // Check click on action item
    for (size_t i = 0; i < m_itemRects.size(); ++i) {
        const auto& r = m_itemRects[i];
        if (x >= r.left && x <= r.right && y >= r.top && y <= r.bottom) {
            TriggerAction(i);
            return true;
        }
    }

    // Click outside closes overlay
    if (x < m_hudRect.left || x > m_hudRect.right || y < m_hudRect.top || y > m_hudRect.bottom) {
        Hide();
        return true;
    }
    return true;
}

bool AiActionOverlay::OnMouseWheel(float delta) {
    if (!m_visible) return false;
    const auto& actions = AI::AiActionManager::Instance().GetActions();
    int count = static_cast<int>(actions.size());
    if (count <= 0) return true;

    if (delta > 0) {
        m_selectedIndex = (m_selectedIndex - 1 + count) % count;
    } else {
        m_selectedIndex = (m_selectedIndex + 1) % count;
    }
    RequestRepaint(QuickView::PaintLayer::Static);
    if (m_hwnd) InvalidateRect(m_hwnd, nullptr, FALSE);
    return true;
}

void AiActionOverlay::Render(ID2D1DeviceContext* dc, float winW, float winH) {
    if (!m_visible || !dc) return;

    CreateDeviceResources(dc);

    const auto& actions = AI::AiActionManager::Instance().GetActions();
    int count = static_cast<int>(actions.size());

    float hudW = 400.0f * m_uiScale;
    float itemH = 46.0f * m_uiScale;
    float headerH = 48.0f * m_uiScale;
    float padding = 12.0f * m_uiScale;
    float hudH = headerH + count * (itemH + 6.0f * m_uiScale) + padding * 1.5f;

    float hudX = (winW - hudW) * 0.5f;
    float hudY = (winH - hudH) * 0.38f; // Golden ratio positioning
    if (hudY < 30.0f) hudY = 30.0f;

    m_hudRect = D2D1::RectF(hudX, hudY, hudX + hudW, hudY + hudH);
    m_itemRects.resize(count);

    bool isLight = IsLightThemeActive();

    // 1. Full-screen Dimmer (Focus spotlight on AI Action overlay)
    D2D1_COLOR_F dimmerClr = isLight ? D2D1::ColorF(0.95f, 0.95f, 0.97f, 0.40f) : D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.45f);
    m_brushBg->SetColor(dimmerClr);
    dc->FillRectangle(D2D1::RectF(0, 0, winW, winH), m_brushBg.Get());

    // 2. Draw Background Panel with Geek Glass or Solid Fallback
    const float cardRadius = 12.0f * m_uiScale;
    if (m_bgCmdList) {
        m_geekGlass.InitializeResources(dc);
        QuickView::UI::GeekGlass::GeekGlassConfig config;
        config.theme = isLight ? QuickView::UI::GeekGlass::ThemeMode::Light : QuickView::UI::GeekGlass::ThemeMode::Dark;
        config.panelBounds = m_hudRect;
        config.cornerRadius = cardRadius;
        config.enableGeekGlass = g_config.EnableGeekGlass;
        config.tintProfile = g_config.GlassTintProfile;
        config.customTintColor = D2D1::ColorF(g_config.GlassCustomTintR, g_config.GlassCustomTintG, g_config.GlassCustomTintB, g_config.GlassTintAlpha);
        config.tintAlpha = g_config.GlassTintAlpha;
        config.specularOpacity = g_config.GlassSpecularOpacity;
        config.blurStandardDeviation = g_config.GlassBlurSigma * m_uiScale;
        config.opacity = g_config.EnableGeekGlass ? (g_config.GlassModalsOpacity / 100.0f) : 0.95f;
        config.strokeWeight = g_config.GetVectorStrokeWeight();
        config.shadowOpacity = g_config.GlassShadowOpacity;
        config.pBackgroundCommandList = m_bgCmdList.Get();
        config.backgroundTransform = m_bgTransform;

        m_geekGlass.DrawGeekGlassPanel(dc, config);

        if (g_config.EnableGeekGlass) {
            float masterOpacity = g_config.GlassModalsOpacity / 100.0f;
            D2D1_COLOR_F fillerColor = isLight ? D2D1::ColorF(0.95f, 0.95f, 0.97f, 1.0f) : D2D1::ColorF(0.08f, 0.08f, 0.10f, 1.0f);
            m_brushBg->SetColor(fillerColor);
            m_brushBg->SetOpacity(masterOpacity);
            dc->FillRoundedRectangle(D2D1::RoundedRect(m_hudRect, cardRadius, cardRadius), m_brushBg.Get());
            m_brushBg->SetOpacity(1.0f);
            m_geekGlass.DrawGeekGlassToppings(dc, config);
        }
    } else {
        m_brushBg->SetOpacity(1.0f);
        m_brushBg->SetColor(isLight ? D2D1::ColorF(0.95f, 0.95f, 0.97f, 0.95f) : D2D1::ColorF(0.12f, 0.12f, 0.14f, 0.95f));
        dc->FillRoundedRectangle(D2D1::RoundedRect(m_hudRect, cardRadius, cardRadius), m_brushBg.Get());
    }
    dc->DrawRoundedRectangle(D2D1::RoundedRect(m_hudRect, cardRadius, cardRadius), m_brushBorder.Get(), 1.0f * m_uiScale);

    // Draw Title Header
    D2D1_RECT_F titleRect = D2D1::RectF(hudX + padding + 6.0f, hudY + padding, hudX + hudW - padding, hudY + headerH);
    std::wstring titleText = L"AI 动作 (AI Actions)";
    dc->DrawText(titleText.c_str(), static_cast<UINT32>(titleText.size()), m_fontTitle.Get(), titleRect, m_brushText.Get());

    // Draw Esc Hint
    D2D1_RECT_F escRect = D2D1::RectF(hudX + hudW - 100.0f * m_uiScale, hudY + padding + 4.0f, hudX + hudW - padding, hudY + headerH);
    std::wstring escText = L"[Esc] 退出";
    dc->DrawText(escText.c_str(), static_cast<UINT32>(escText.size()), m_fontDetail.Get(), escRect, m_brushTextDim.Get());

    // Draw Items
    float curY = hudY + headerH;
    for (int i = 0; i < count; ++i) {
        const auto& act = actions[i];
        D2D1_RECT_F itemR = D2D1::RectF(hudX + padding, curY, hudX + hudW - padding, curY + itemH);
        m_itemRects[i] = itemR;

        D2D1_ROUNDED_RECT itemRounded = D2D1::RoundedRect(itemR, 8.0f * m_uiScale, 8.0f * m_uiScale);

        // Background highlight
        if (i == m_selectedIndex) {
            dc->FillRoundedRectangle(itemRounded, m_brushCardSelected.Get());
            dc->DrawRoundedRectangle(itemRounded, m_brushAccent.Get(), 1.5f * m_uiScale);
        } else if (i == m_hoverIndex) {
            dc->FillRoundedRectangle(itemRounded, m_brushCardHover.Get());
            dc->DrawRoundedRectangle(itemRounded, m_brushBorder.Get(), 1.0f * m_uiScale);
        } else {
            dc->FillRoundedRectangle(itemRounded, m_brushCard.Get());
            dc->DrawRoundedRectangle(itemRounded, m_brushBorder.Get(), 0.8f * m_uiScale);
        }

        // Draw Keycap Badge [1]..[9]
        bool isLight = IsLightThemeActive();
        float badgeW = 22.0f * m_uiScale;
        float badgeH = 22.0f * m_uiScale;
        float badgeX = itemR.left + 10.0f * m_uiScale;
        float badgeY = itemR.top + (itemH - badgeH) * 0.5f;
        D2D1_RECT_F keycapRect = D2D1::RectF(badgeX, badgeY, badgeX + badgeW, badgeY + badgeH);
        float keycapRadius = 4.5f * m_uiScale;

        // Keycap 3D bottom shadow
        D2D1_ROUNDED_RECT shadowR = D2D1::RoundedRect(
            D2D1::RectF(keycapRect.left, keycapRect.top + 1.2f * m_uiScale, keycapRect.right, keycapRect.bottom + 1.2f * m_uiScale),
            keycapRadius, keycapRadius);
        D2D1_COLOR_F shadowClr = isLight ? D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.18f) : D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.45f);
        m_brushKeyBadge->SetColor(shadowClr);
        dc->FillRoundedRectangle(shadowR, m_brushKeyBadge.Get());

        // Keycap cap body
        D2D1_ROUNDED_RECT bodyR = D2D1::RoundedRect(keycapRect, keycapRadius, keycapRadius);
        D2D1_COLOR_F capBodyClr = isLight 
            ? (i == m_selectedIndex ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f) : D2D1::ColorF(0.95f, 0.95f, 0.97f, 1.0f))
            : (i == m_selectedIndex ? D2D1::ColorF(0.24f, 0.26f, 0.32f, 1.0f) : D2D1::ColorF(0.18f, 0.18f, 0.22f, 1.0f));
        m_brushKeyBadge->SetColor(capBodyClr);
        dc->FillRoundedRectangle(bodyR, m_brushKeyBadge.Get());

        // Keycap border
        D2D1_COLOR_F capBorderClr = isLight
            ? (i == m_selectedIndex ? D2D1::ColorF(0.0f, 0.45f, 0.9f, 0.6f) : D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.15f))
            : (i == m_selectedIndex ? D2D1::ColorF(0.3f, 0.6f, 1.0f, 0.6f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.14f));
        m_brushBorder->SetColor(capBorderClr);
        dc->DrawRoundedRectangle(bodyR, m_brushBorder.Get(), 1.0f * m_uiScale);

        // Keycap number text (perfectly centered)
        wchar_t keyBuf[4] = { 0 };
        if (i < 9) swprintf_s(keyBuf, L"%d", i + 1);
        else swprintf_s(keyBuf, L"•");
        ID2D1SolidColorBrush* numBrush = (i == m_selectedIndex) ? m_brushAccent.Get() : (isLight ? m_brushText.Get() : m_brushTextDim.Get());
        dc->DrawText(keyBuf, static_cast<UINT32>(wcslen(keyBuf)), m_fontBadge.Get(), keycapRect, numBrush);

        // Draw Action Name
        float textX = badgeX + badgeW + 10.0f * m_uiScale;
        D2D1_RECT_F textR = D2D1::RectF(textX, itemR.top + 6.0f * m_uiScale, itemR.right - 80.0f * m_uiScale, itemR.top + itemH - 4.0f * m_uiScale);
        dc->DrawText(act.name.c_str(), static_cast<UINT32>(act.name.size()), m_fontItem.Get(), textR, m_brushText.Get());

        // Draw Scope / Detail Tag
        std::wstring tagStr;
        if (act.scopeMode == AI::ScopeMode::CropAndBlend) tagStr = L"选区修补";
        else if (act.scopeMode == AI::ScopeMode::ForceFullImage) tagStr = L"全图生成";
        else tagStr = L"自适应";

        D2D1_RECT_F tagR = D2D1::RectF(itemR.right - 75.0f * m_uiScale, itemR.top + (itemH - 18.0f * m_uiScale) * 0.5f, itemR.right - 8.0f * m_uiScale, itemR.top + itemH);
        dc->DrawText(tagStr.c_str(), static_cast<UINT32>(tagStr.size()), m_fontDetail.Get(), tagR, m_brushTextDim.Get());

        curY += itemH + 6.0f * m_uiScale;
    }
}

} // namespace QuickView::UI
