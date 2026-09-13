#pragma once
// ============================================================================
// AiActionManager.h - High Performance AI Action & Profile Management Engine
// ============================================================================
// Features:
// 1. Separation of Concerns: Model Profiles vs AI Actions
// 2. DPAPI Security: Windows CryptProtectData for local credentials
// 3. Robust WinHTTP: Asynchronous execution, Task ID tracking, instant cancel/fuse
// 4. Inpainting Preprocessing: Adaptive downsampling & Alpha edge feathering
// 5. Zero-Exception Architecture: std::expected / error codes throughout
// ============================================================================

#include "pch.h"
#include <vector>
#include <string>
#include <memory>
#include <atomic>
#include <mutex>
#include <functional>
#include <winhttp.h>
#include <wincrypt.h>
#include "AiActionTypes.h"
#include "AppContext.h"

namespace QuickView::AI {

class AiActionManager {
public:
    static AiActionManager& Instance();

    // --- Profile & Action Configuration Management ---
    bool Init();
    bool LoadConfig();
    bool SaveConfig();
    bool ReloadConfig();
    void ResetToDefaults();
    std::wstring GetConfigFilePath() const;

    std::vector<ModelProfile>& GetProfiles() { return m_profiles; }
    const std::vector<ModelProfile>& GetProfiles() const { return m_profiles; }

    std::vector<ActionDesc>& GetActions() { return m_actions; }
    const std::vector<ActionDesc>& GetActions() const { return m_actions; }

    const ModelProfile* FindProfile(std::string_view profileId) const;
    const ModelProfile* GetDefaultProfile() const;

    void SetDefaultProfileId(std::string_view id);
    const std::string& GetDefaultProfileId() const { return m_defaultProfileId; }

    void SetLastActionId(std::string_view id);
    const std::string& GetLastActionId() const { return m_lastActionId; }

    // --- Security: Windows DPAPI En/Decryption ---
    static std::string EncryptApiKey(std::string_view plainText);
    static std::string DecryptApiKey(std::string_view cipherBase64);

    // --- Execution & Task Lifecycle ---
    uint64_t ExecuteAction(const ActionDesc& action, HWND hwnd, std::function<void(const ExecutionResult&)> onComplete);
    void CancelCurrentTask();
    bool IsRunning() const { return m_isRunning.load(); }
    uint64_t GetCurrentTaskId() const { return m_currentTaskId.load(); }

    // --- Dynamic Model List Fetching ---
    void FetchModelsAsync(
        std::string baseUrl,
        std::string apiKey,
        ApiProtocol protocol,
        std::function<void(bool success, const std::vector<std::string>& models, const std::wstring& errorMsg)> onComplete);

    // --- Connection Testing & Latency Probing ---
    void TestConnectionAsync(
        std::string baseUrl,
        std::string apiKey,
        ApiProtocol protocol,
        std::function<void(bool success, int statusCode, int latencyMs, const std::wstring& message)> onComplete);

    // --- Error Formatting Helper ---
    static std::wstring FormatAiErrorMessage(DWORD statusCode, std::string_view responseBody);

    // --- Image Processing & Inpainting Helper ---
    // Downsamples BGRA image buffer to fit maxDim while maintaining aspect ratio and aligning to step
    static bool PrepareImageBuffer(
        const uint8_t* srcBgra, int srcW, int srcH, int srcStride,
        MaxResolution maxRes, uint32_t stepAlignment,
        std::vector<uint8_t>& outBgra, int& outW, int& outH);

    // Encodes BGRA raw pixels into PNG format stream in memory via WIC
    static bool EncodeToPngMemory(
        const uint8_t* bgra, int width, int height, int stride,
        std::vector<uint8_t>& outPngBytes);

    // Blits generated sub-image back onto original buffer with 3~5px alpha feathering
    static void BlendFeatheredSubImage(
        uint8_t* dstBgra, int dstW, int dstH, int dstStride,
        const uint8_t* srcBgra, int srcW, int srcH, int srcStride,
        int targetX, int targetY, int featherPixels = 4);

private:
    AiActionManager();
    ~AiActionManager();

    void InitDefaultTemplates();

    // Worker thread function
    void WorkerThread(uint64_t taskId, ActionDesc action, ModelProfile profile, HWND hwnd, std::function<void(const ExecutionResult&)> callback);

    std::vector<ModelProfile> m_profiles;
    std::vector<ActionDesc> m_actions;
    std::string m_defaultProfileId;
    std::string m_lastActionId;

    bool m_initialized = false;
    std::atomic<bool> m_isRunning{ false };
    std::atomic<uint64_t> m_currentTaskId{ 0 };
    std::mutex m_taskMutex;
    HINTERNET m_activeSession = nullptr;
    HINTERNET m_activeConnect = nullptr;
    HINTERNET m_activeRequest = nullptr;
};

} // namespace QuickView::AI
