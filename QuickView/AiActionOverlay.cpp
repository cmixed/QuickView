#include "pch.h"
#include "AiActionOverlay.h"
#include "AiActionManager.h"
#include "AppContext.h"
#include "CompareController.h"
#include "ThemeSystem.h"
#include "OSDState.h"
#include "EditState.h"
#include "AppStrings.h"
#include <thread>
#include <chrono>
#include <cmath>

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

    const auto* profile = AI::AiActionManager::Instance().FindProfile(act.modelProfileId);
    if (!profile) profile = AI::AiActionManager::Instance().GetDefaultProfile();
    bool isSdWebUi = profile && (profile->protocol == AI::ApiProtocol::StabilityInpaint);
    std::string baseUrl = profile ? profile->baseUrl : "";

    HWND hwnd = m_hwnd ? m_hwnd : g_mainHwnd;

    wchar_t initMsg[256] = { 0 };
    swprintf_s(initMsg, L"AI: %s 准备中 (Esc取消)...", act.name.c_str());
    g_osd.StartPersistentTask(hwnd, initMsg, D2D1::ColorF(D2D1::ColorF::White), OSDPosition::Bottom, 0.05f);

    auto taskFinished = std::make_shared<std::atomic<bool>>(false);

    uint64_t currentTaskId = AI::AiActionManager::Instance().ExecuteAction(act, hwnd, [act, taskFinished](const AI::ExecutionResult& res) {
        taskFinished->store(true);
        if (!res.success) {
            g_osd.EndPersistentTask(g_mainHwnd);
            AI::AiActionManager::ShowAiErrorDialog(g_mainHwnd, res);
            return;
        }

        if (!res.resultImageData.empty()) {
            auto* pData = new AI::AsyncAiImageResult();
            pData->imageData = std::move(res.resultImageData);
            pData->actionName = act.name;
            pData->width = res.imageWidth;
            pData->height = res.imageHeight;
            PostMessageW(g_mainHwnd, AI::WM_AI_ACTION_COMPLETED, 0, reinterpret_cast<LPARAM>(pData));
        } else if (!res.textContent.empty()) {
            // Execution succeeded with Text Content
            std::wstring displayMsg = act.name + L": " + res.textContent;
            g_osd.EndPersistentTask(g_mainHwnd, displayMsg, false, D2D1::ColorF(D2D1::ColorF::LightGreen), 8000);
        } else {
            g_osd.EndPersistentTask(g_mainHwnd);
        }
    });

    std::wstring actName = act.name;
    std::thread([hwnd, actName, baseUrl, isSdWebUi, currentTaskId, taskFinished]() {
        auto startTime = std::chrono::steady_clock::now();
        while (!taskFinished->load() && AI::AiActionManager::Instance().IsRunning() && AI::AiActionManager::Instance().GetCurrentTaskId() == currentTaskId) {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            if (taskFinished->load() || !AI::AiActionManager::Instance().IsRunning() || AI::AiActionManager::Instance().GetCurrentTaskId() != currentTaskId) {
                break;
            }
            auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startTime).count();
            int elapsedSec = static_cast<int>(elapsedMs / 1000);

            if (isSdWebUi) {
                AI::AiActionManager::SdProgressInfo prog;
                if (AI::AiActionManager::PollSdProgress(baseUrl, prog) && prog.totalSteps > 0 && prog.progress > 0.0f) {
                    wchar_t sdMsg[256] = { 0 };
                    int pct = static_cast<int>(prog.progress * 100.0f);
                    if (prog.eta > 0.0f) {
                        swprintf_s(sdMsg, L"AI 采样中: %d%% (%d/%d步, 约剩%.0fs) [耗时%ds, Esc取消]",
                            pct, prog.currentStep, prog.totalSteps, prog.eta, elapsedSec);
                    } else {
                        swprintf_s(sdMsg, L"AI 采样中: %d%% (%d/%d步) [耗时%ds, Esc取消]",
                            pct, prog.currentStep, prog.totalSteps, elapsedSec);
                    }
                    g_osd.UpdatePersistentTask(hwnd, sdMsg, (std::max)(0.05f, (std::min)(prog.progress, 0.99f)));
                    continue;
                }
            }

            // Fallback for cloud LLMs or SD before sampling starts (warmup / loading model)
            wchar_t cloudMsg[256] = { 0 };
            if (elapsedSec < 3) {
                swprintf_s(cloudMsg, L"AI 连接中: %s (Esc取消)...", actName.c_str());
            } else {
                swprintf_s(cloudMsg, L"AI 生成中: %s [已耗时 %ds, Esc取消]...", actName.c_str(), elapsedSec);
            }

            // Smooth asymptotic progress for cloud LLM: starts at 5%, slowly approaches 95%
            float fakeProgress = 1.0f - std::exp(-static_cast<float>(elapsedSec) / 25.0f);
            fakeProgress = (std::max)(0.05f, (std::min)(fakeProgress, 0.95f));

            g_osd.UpdatePersistentTask(hwnd, cloudMsg, fakeProgress);
        }
    }).detach();
}

void AiActionOverlay::StartInpaintSelection() {
    Hide();
    g_cropState.Reset();
    g_cropState.Mode = RegionInteractionMode::AiInpaint;
    g_cropState.IsActive = true;
    g_cropState.IsDragging = false;
    g_cropState.InpaintInputFocused = false;
    g_cropState.IsQuickActionVisible = false;
    g_cropState.CropLeft = 0.0f;
    g_cropState.CropTop = 0.0f;
    g_cropState.CropRight = 0.0f;
    g_cropState.CropBottom = 0.0f;

    const wchar_t* guide = L"请使用鼠标左键框选重绘区域";
    g_osd.Show(m_hwnd ? m_hwnd : g_mainHwnd, guide, false, false, D2D1::ColorF(0.4f, 0.8f, 1.0f), OSDPosition::Bottom, 4000);
    RequestRepaint(QuickView::PaintLayer::All);
}

bool AiActionOverlay::OnKeyDown(WPARAM key) {
    if (!m_visible) return false;

    const auto& actions = AI::AiActionManager::Instance().GetActions();
    int count = static_cast<int>(actions.size());

    // 1. Esc -> Close
    if (key == VK_ESCAPE) {
        if (AI::AiActionManager::Instance().IsRunning()) {
            AI::AiActionManager::Instance().CancelCurrentTask();
            g_osd.EndPersistentTask(m_hwnd ? m_hwnd : g_mainHwnd, L"AI 任务已取消", false, D2D1::ColorF(D2D1::ColorF::LightSalmon), 1500);
        }
        Hide();
        return true;
    }

    // 2. Direct Shortcut Key '0' or 'I' -> Trigger Inpainting Selection
    if (key == '0' || key == 'I' || key == 'i') {
        StartInpaintSelection();
        return true;
    }

    // 3. Direct Numeric Keys 1..9 -> Trigger instantly
    if (key >= '1' && key <= '9') {
        int idx = static_cast<int>(key - '1');
        if (idx < count) {
            TriggerAction(idx);
            return true;
        }
    }

    // 4. Arrow Keys / Rotation Cycle
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

    // 5. Enter -> Trigger Selected
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
    bool prevInpaintHover = m_hoverInpaintCard;
    m_hoverIndex = -1;
    m_hoverInpaintCard = false;

    if (x >= m_inpaintCardRect.left && x <= m_inpaintCardRect.right &&
        y >= m_inpaintCardRect.top && y <= m_inpaintCardRect.bottom) {
        m_hoverInpaintCard = true;
    }

    for (size_t i = 0; i < m_itemRects.size(); ++i) {
        const auto& r = m_itemRects[i];
        if (x >= r.left && x <= r.right && y >= r.top && y <= r.bottom) {
            m_hoverIndex = static_cast<int>(i);
            break;
        }
    }

    if (m_hoverIndex != prevHover || m_hoverInpaintCard != prevInpaintHover) {
        RequestRepaint(QuickView::PaintLayer::Static);
        if (m_hwnd) InvalidateRect(m_hwnd, nullptr, FALSE);
    }
    return true;
}

bool AiActionOverlay::OnLButtonDown(float x, float y) {
    if (!m_visible) return false;

    // Check click on inpaint hero card
    if (x >= m_inpaintCardRect.left && x <= m_inpaintCardRect.right &&
        y >= m_inpaintCardRect.top && y <= m_inpaintCardRect.bottom) {
        StartInpaintSelection();
        return true;
    }

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
    float inpaintCardH = 48.0f * m_uiScale;
    float headerH = 48.0f * m_uiScale;
    float padding = 12.0f * m_uiScale;
    float hudH = headerH + inpaintCardH + 8.0f * m_uiScale + count * (itemH + 6.0f * m_uiScale) + padding * 1.5f;

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
    const wchar_t* titleText = AppStrings::AiAction_Title ? AppStrings::AiAction_Title : L"AI Actions";
    dc->DrawText(titleText, static_cast<UINT32>(wcslen(titleText)), m_fontTitle.Get(), titleRect, m_brushText.Get());

    // Draw Esc Hint
    D2D1_RECT_F escRect = D2D1::RectF(hudX + hudW - 100.0f * m_uiScale, hudY + padding + 4.0f, hudX + hudW - padding, hudY + headerH);
    const wchar_t* escText = AppStrings::AiAction_EscHint ? AppStrings::AiAction_EscHint : L"[Esc] Close";
    dc->DrawText(escText, static_cast<UINT32>(wcslen(escText)), m_fontDetail.Get(), escRect, m_brushTextDim.Get());

    float curY = hudY + headerH;

    // Draw Inpaint Hero Card
    m_inpaintCardRect = D2D1::RectF(hudX + padding, curY, hudX + hudW - padding, curY + inpaintCardH);
    D2D1_ROUNDED_RECT inpaintRounded = D2D1::RoundedRect(m_inpaintCardRect, 8.0f * m_uiScale, 8.0f * m_uiScale);

    if (m_hoverInpaintCard) {
        dc->FillRoundedRectangle(inpaintRounded, m_brushCardHover.Get());
        dc->DrawRoundedRectangle(inpaintRounded, m_brushAccent.Get(), 1.5f * m_uiScale);
    } else {
        dc->FillRoundedRectangle(inpaintRounded, m_brushCard.Get());
        dc->DrawRoundedRectangle(inpaintRounded, m_brushBorder.Get(), 1.0f * m_uiScale);
    }

    // Badge [ 0 ]
    float badgeW = 22.0f * m_uiScale;
    float badgeH = 22.0f * m_uiScale;
    float badgeX = m_inpaintCardRect.left + 10.0f * m_uiScale;
    float badgeY = m_inpaintCardRect.top + (inpaintCardH - badgeH) * 0.5f;
    D2D1_RECT_F keycapRect = D2D1::RectF(badgeX, badgeY, badgeX + badgeW, badgeY + badgeH);
    D2D1_ROUNDED_RECT bodyR = D2D1::RoundedRect(keycapRect, 4.5f * m_uiScale, 4.5f * m_uiScale);
    m_brushKeyBadge->SetColor(isLight ? D2D1::ColorF(0.92f, 0.94f, 0.98f, 1.0f) : D2D1::ColorF(0.20f, 0.22f, 0.28f, 1.0f));
    dc->FillRoundedRectangle(bodyR, m_brushKeyBadge.Get());
    dc->DrawRoundedRectangle(bodyR, m_brushBorder.Get(), 1.0f * m_uiScale);
    dc->DrawText(L"0", 1, m_fontBadge.Get(), keycapRect, m_brushAccent.Get());

    // Inpaint Card Title & Subtitle
    float textX = badgeX + badgeW + 10.0f * m_uiScale;
    D2D1_RECT_F textR = D2D1::RectF(textX, m_inpaintCardRect.top + 5.0f * m_uiScale, m_inpaintCardRect.right - 85.0f * m_uiScale, m_inpaintCardRect.top + 26.0f * m_uiScale);
    const wchar_t* inpaintTitle = L"✨ 选区局部重绘 (AI Inpaint)";
    dc->DrawText(inpaintTitle, static_cast<UINT32>(wcslen(inpaintTitle)), m_fontItem.Get(), textR, m_brushText.Get());

    D2D1_RECT_F subR = D2D1::RectF(textX, m_inpaintCardRect.top + 26.0f * m_uiScale, m_inpaintCardRect.right - 85.0f * m_uiScale, m_inpaintCardRect.bottom - 4.0f * m_uiScale);
    const wchar_t* inpaintSub = L"框选局部画面无痕消除或提示词替换";
    dc->DrawText(inpaintSub, static_cast<UINT32>(wcslen(inpaintSub)), m_fontDetail.Get(), subR, m_brushTextDim.Get());

    // Tag
    D2D1_RECT_F inpaintTagR = D2D1::RectF(m_inpaintCardRect.right - 80.0f * m_uiScale, m_inpaintCardRect.top + (inpaintCardH - 18.0f * m_uiScale) * 0.5f, m_inpaintCardRect.right - 8.0f * m_uiScale, m_inpaintCardRect.bottom);
    const wchar_t* tagInpaint = L"框选模式";
    dc->DrawText(tagInpaint, static_cast<UINT32>(wcslen(tagInpaint)), m_fontDetail.Get(), inpaintTagR, m_brushAccent.Get());

    curY += inpaintCardH + 8.0f * m_uiScale;

    // Draw Items
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
        const wchar_t* tagStr = L"";
        if (act.scopeMode == AI::ScopeMode::CropAndBlend) {
            tagStr = AppStrings::AiAction_ScopeCropAndBlend ? AppStrings::AiAction_ScopeCropAndBlend : L"Inpaint";
        } else if (act.scopeMode == AI::ScopeMode::ForceFullImage) {
            tagStr = AppStrings::AiAction_ScopeForceFull ? AppStrings::AiAction_ScopeForceFull : L"Full";
        } else {
            tagStr = AppStrings::AiAction_ScopeAuto ? AppStrings::AiAction_ScopeAuto : L"Auto";
        }

        D2D1_RECT_F tagR = D2D1::RectF(itemR.right - 95.0f * m_uiScale, itemR.top + (itemH - 18.0f * m_uiScale) * 0.5f, itemR.right - 8.0f * m_uiScale, itemR.top + itemH);
        dc->DrawText(tagStr, static_cast<UINT32>(wcslen(tagStr)), m_fontDetail.Get(), tagR, m_brushTextDim.Get());

        curY += itemH + 6.0f * m_uiScale;
    }
}

} // namespace QuickView::UI
