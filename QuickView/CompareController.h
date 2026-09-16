#pragma once

#include <windows.h>
#include <d2d1_2.h>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include "AppContext.h"
#include "CoroutineTypes.h"

struct CompareSlotCallback {
    void* userCtx = nullptr;
    void (*pfn)(void* userCtx, bool success) = nullptr;
    void (*cleanup)(void* userCtx) = nullptr;

    constexpr CompareSlotCallback() noexcept = default;

    ~CompareSlotCallback() {
        Reset();
    }

    CompareSlotCallback(CompareSlotCallback&& other) noexcept
        : userCtx(other.userCtx), pfn(other.pfn), cleanup(other.cleanup) {
        other.userCtx = nullptr;
        other.pfn = nullptr;
        other.cleanup = nullptr;
    }

    CompareSlotCallback(CompareSlotCallback& other) noexcept
        : userCtx(other.userCtx), pfn(other.pfn), cleanup(other.cleanup) {
        other.userCtx = nullptr;
        other.pfn = nullptr;
        other.cleanup = nullptr;
    }

    CompareSlotCallback& operator=(CompareSlotCallback&& other) noexcept {
        if (this != &other) {
            Reset();
            userCtx = other.userCtx;
            pfn = other.pfn;
            cleanup = other.cleanup;
            other.userCtx = nullptr;
            other.pfn = nullptr;
            other.cleanup = nullptr;
        }
        return *this;
    }

    CompareSlotCallback& operator=(CompareSlotCallback& other) noexcept {
        if (this != &other) {
            Reset();
            userCtx = other.userCtx;
            pfn = other.pfn;
            cleanup = other.cleanup;
            other.userCtx = nullptr;
            other.pfn = nullptr;
            other.cleanup = nullptr;
        }
        return *this;
    }

    template <typename F>
        requires (!std::is_same_v<std::remove_cvref_t<F>, CompareSlotCallback> && std::is_invocable_v<F, bool>)
    CompareSlotCallback(F&& f) {
        using DecayedF = std::decay_t<F>;
        if constexpr (std::is_pointer_v<DecayedF>) {
            userCtx = reinterpret_cast<void*>(f);
            pfn = [](void* u, bool success) {
                auto fn = reinterpret_cast<DecayedF>(u);
                if (fn) fn(success);
            };
            cleanup = nullptr;
        } else if constexpr (std::is_empty_v<DecayedF>) {
            pfn = [](void*, bool success) {
                DecayedF{}(success);
            };
            cleanup = nullptr;
        } else {
            auto* p = new DecayedF(std::forward<F>(f));
            userCtx = p;
            pfn = [](void* u, bool success) {
                (*static_cast<DecayedF*>(u))(success);
            };
            cleanup = [](void* u) {
                delete static_cast<DecayedF*>(u);
            };
        }
    }

    void Invoke(bool success) const {
        if (pfn) {
            pfn(userCtx, success);
        }
    }

    void Reset() {
        if (cleanup && userCtx) {
            cleanup(userCtx);
            userCtx = nullptr;
        }
        pfn = nullptr;
        cleanup = nullptr;
    }

    explicit operator bool() const noexcept {
        return pfn != nullptr;
    }
};

class CompareController {
public:
    explicit CompareController(AppContext& context);
    ~CompareController() = default;

    std::optional<LRESULT> HandleMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    void Render(ID2D1DeviceContext* ctx);

    bool RenderComposite(HWND hwnd);
    void MarkDirty();
    void EnterMode(HWND hwnd);
    void EnterSrCompareMode(HWND hwnd);
    void ExitMode(HWND hwnd);
    bool IsActive() const;
    
    ComparePane HitTest(HWND hwnd, POINT ptClient) const;
    D2D1_RECT_F GetViewport(HWND hwnd, ComparePane pane) const;
    float GetSplitRatio() const;

    void CaptureCurrentImageAsLeft();
    FireAndForget LoadImageIntoLeftSlot(HWND hwnd, std::wstring path, CompareSlotCallback callback = {});
    void ReloadPaneForDisplayChange(HWND hwnd, ComparePane pane);
    
    void UpdateRawButton();
    bool GetPaneRawState(ComparePane pane, bool& isRaw, bool& isFullDecode) const;
    void RefreshRawUI(HWND hwnd);
    void CenterDialogOnPaneIfNeeded(HWND hwnd, ComparePane pane);
    void ApplyZoomStep(HWND hwnd, float delta, bool fineInterval);

    bool HitTestEdgeNav(HWND hwnd, POINT ptClient) const;
    void UpdateEdgeHoverStates(HWND hwnd, POINT ptClient);
    bool HitTestEdgeZone(HWND hwnd, POINT ptClient) const;
    int HandleEdgeNavClick(HWND hwnd, POINT ptClient);
    bool IsNearCompareDivider(HWND hwnd, POINT ptClient, float threshold = 6.0f) const;

private:
    AppContext& m_context;
    HWND m_hwnd = nullptr;

    // Internal message handlers
    std::optional<LRESULT> OnLButtonDown(HWND hwnd, int x, int y);
    std::optional<LRESULT> OnLButtonUp(HWND hwnd, int x, int y);
    std::optional<LRESULT> OnMouseMove(HWND hwnd, int x, int y);
    std::optional<LRESULT> OnKeyDown(HWND hwnd, WPARAM key);
};

inline bool IsCompareModeActive() {
    return AppContext::GetInstance().CompareCtrl && AppContext::GetInstance().CompareCtrl->IsActive();
}

inline void RefreshCompareRawUI(HWND hwnd) {
    if (AppContext::GetInstance().CompareCtrl) {
        AppContext::GetInstance().CompareCtrl->RefreshRawUI(hwnd);
    }
}

int HitTestNavButtonInPane(POINT pt, const D2D1_RECT_F& rect);
int ComputeEdgeHoverForPane(POINT pt, const D2D1_RECT_F& rect);
