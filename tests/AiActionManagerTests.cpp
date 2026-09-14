#include <gtest/gtest.h>
#include "AiActionManager.h"
#include "OSDState.h"
#include "AppStrings.h"
#include <windows.h>
#include <shlwapi.h>
#include <shlobj.h>

// Stubs for test harness
OSDState g_osd;
std::wstring g_imagePath;
std::wstring GetCurrentActiveImagePath() {
    return g_imagePath;
}

std::wstring GetConfigPath([[maybe_unused]] bool forcePortableCheck) {
    wchar_t tempDir[MAX_PATH];
    GetTempPathW(MAX_PATH, tempDir);
    return std::wstring(tempDir) + L"QuickView_Test.ini";
}

class AiActionManagerTest : public ::testing::Test {
protected:
    std::wstring m_testJsonPath;

    void SetUp() override {
        AppStrings::SetLanguage(AppStrings::Language::ChineseSimplified);
        m_testJsonPath = QuickView::AI::AiActionManager::Instance().GetConfigFilePath();
        DeleteFileW(m_testJsonPath.c_str());
        QuickView::AI::AiActionManager::Instance().ResetToDefaults();
    }

    void TearDown() override {
        DeleteFileW(m_testJsonPath.c_str());
    }
};

TEST_F(AiActionManagerTest, DpapiEncryptionRoundTrip) {
    std::string testKey = "AIzaSyD-TestKey_1234567890_!@#$%^&*()_+";
    std::string encrypted = QuickView::AI::AiActionManager::EncryptApiKey(testKey);
    EXPECT_FALSE(encrypted.empty());
    EXPECT_NE(encrypted, testKey);

    std::string decrypted = QuickView::AI::AiActionManager::DecryptApiKey(encrypted);
    EXPECT_EQ(decrypted, testKey);
}

TEST_F(AiActionManagerTest, EmptyApiKeyHandling) {
    EXPECT_TRUE(QuickView::AI::AiActionManager::EncryptApiKey("").empty());
    EXPECT_TRUE(QuickView::AI::AiActionManager::DecryptApiKey("").empty());
}

TEST_F(AiActionManagerTest, PathDerivedFromConfigPath) {
    std::wstring cfg = QuickView::AI::AiActionManager::Instance().GetConfigFilePath();
    EXPECT_FALSE(cfg.empty());
    EXPECT_NE(cfg.find(L"ai_actions.json"), std::wstring::npos);

    std::wstring ini = GetConfigPath(false);
    wchar_t iniDir[MAX_PATH] = { 0 };
    wcscpy_s(iniDir, ini.c_str());
    PathRemoveFileSpecW(iniDir);

    wchar_t jsonDir[MAX_PATH] = { 0 };
    wcscpy_s(jsonDir, cfg.c_str());
    PathRemoveFileSpecW(jsonDir);

    EXPECT_EQ(std::wstring(iniDir), std::wstring(jsonDir));
}

TEST_F(AiActionManagerTest, ConfigSerializationRoundTripWithDeepCopy) {
    auto& mgr = QuickView::AI::AiActionManager::Instance();
    auto& profiles = mgr.GetProfiles();
    ASSERT_FALSE(profiles.empty());

    // Set custom values with Unicode and special characters
    profiles[0].displayName = L"自定义模型服务商 测试";
    profiles[0].baseUrl = "https://api.example.com/v1/";
    profiles[0].defaultModel = "custom-vision-pro";
    profiles[0].encryptedApiKey = QuickView::AI::AiActionManager::EncryptApiKey("sk-secret-test-key");
    profiles[0].fetchedModels = { "model-alpha", "model-beta", "custom-vision-pro" };

    auto& actions = mgr.GetActions();
    ASSERT_FALSE(actions.empty());
    actions[0].name = L"超分辨率画质精修";
    actions[0].promptTemplate = L"Line 1: Ultra high resolution.\nLine 2: 8k masterpiece, photorealistic.";
    actions[0].negativePrompt = L"blurry, artifacts, bad quality";
    actions[0].samplingSteps = 30;
    actions[0].cfgScale = 8.5f;
    actions[0].aspectRatio = QuickView::AI::OutputAspectRatio::Landscape_16_9;
    actions[0].targetResolution = QuickView::AI::TargetResolution::Res_4K;

    EXPECT_TRUE(mgr.SaveConfig());

    // Reload and verify
    EXPECT_TRUE(mgr.ReloadConfig());

    const auto& reloadedProfiles = mgr.GetProfiles();
    ASSERT_FALSE(reloadedProfiles.empty());
    EXPECT_EQ(reloadedProfiles[0].displayName, L"自定义模型服务商 测试");
    EXPECT_EQ(reloadedProfiles[0].baseUrl, "https://api.example.com/v1/");
    EXPECT_EQ(reloadedProfiles[0].defaultModel, "custom-vision-pro");
    EXPECT_EQ(reloadedProfiles[0].fetchedModels.size(), 3);
    EXPECT_EQ(reloadedProfiles[0].fetchedModels[0], "model-alpha");

    std::string decKey = QuickView::AI::AiActionManager::DecryptApiKey(reloadedProfiles[0].encryptedApiKey);
    EXPECT_EQ(decKey, "sk-secret-test-key");

    const auto& reloadedActions = mgr.GetActions();
    ASSERT_FALSE(reloadedActions.empty());
    EXPECT_EQ(reloadedActions[0].name, L"超分辨率画质精修");
    EXPECT_EQ(reloadedActions[0].promptTemplate, L"Line 1: Ultra high resolution.\nLine 2: 8k masterpiece, photorealistic.");
    EXPECT_EQ(reloadedActions[0].negativePrompt, L"blurry, artifacts, bad quality");
    EXPECT_EQ(reloadedActions[0].samplingSteps, 30);
    EXPECT_FLOAT_EQ(reloadedActions[0].cfgScale, 8.5f);
    EXPECT_EQ(reloadedActions[0].aspectRatio, QuickView::AI::OutputAspectRatio::Landscape_16_9);
    EXPECT_EQ(reloadedActions[0].targetResolution, QuickView::AI::TargetResolution::Res_4K);
}

TEST_F(AiActionManagerTest, ResetToDefaultsRestoresCleanState) {
    auto& mgr = QuickView::AI::AiActionManager::Instance();
    mgr.ResetToDefaults();

    const auto& profiles = mgr.GetProfiles();
    ASSERT_FALSE(profiles.empty());
    EXPECT_EQ(profiles[0].displayName, L"Google Gemini");
    EXPECT_EQ(profiles[0].defaultModel, "");

    const auto& actions = mgr.GetActions();
    ASSERT_FALSE(actions.empty());
    EXPECT_EQ(actions[0].name, L"AI Detail Enhancement & Texture Refine");
}

TEST_F(AiActionManagerTest, FormatAiErrorMessageUnwrapsArrayAndRemovesRawJson) {
    // Test unwrapping 429 error returned as JSON array
    std::string_view jsonArray429 = R"([
  {
    "error": {
      "code": 429,
      "message": "You exceeded your current quota, please check your plan and billing details.",
      "status": "RESOURCE_EXHAUSTED"
    }
  }
])";
    std::wstring msg429 = QuickView::AI::AiActionManager::FormatAiErrorMessage(429, jsonArray429);
    EXPECT_NE(msg429.find(L"HTTP 429"), std::wstring::npos);
    EXPECT_NE(msg429.find(L"You exceeded your current quota"), std::wstring::npos);
    EXPECT_NE(msg429.find(L"Hint: Check your API account balance"), std::wstring::npos);
    // Crucial: Ensure NO raw JSON braces or brackets are leaked into user display
    EXPECT_EQ(msg429.find(L'{'), std::wstring::npos);
    EXPECT_EQ(msg429.find(L'}'), std::wstring::npos);
    EXPECT_EQ(msg429.find(L'['), std::wstring::npos);
    EXPECT_EQ(msg429.find(L']'), std::wstring::npos);

    // Test 503 error formatting
    std::string_view json503 = R"({"error": {"message": "Model is overloaded"}})";
    std::wstring msg503 = QuickView::AI::AiActionManager::FormatAiErrorMessage(503, json503);
    EXPECT_NE(msg503.find(L"HTTP 503"), std::wstring::npos);
    EXPECT_NE(msg503.find(L"Model is overloaded"), std::wstring::npos);
    EXPECT_NE(msg503.find(L"Remote AI server is overloaded"), std::wstring::npos);
    EXPECT_EQ(msg503.find(L'{'), std::wstring::npos);
    EXPECT_EQ(msg503.find(L'}'), std::wstring::npos);
}

TEST_F(AiActionManagerTest, UniversalSemanticExtractionHandlesNestedFastApiValidationArray) {
    // Test universal extraction on nested FastAPI validation array
    std::string_view fastapiNested = R"({
  "detail": [
    {
      "loc": ["body", "init_images"],
      "msg": "Field required: init_images",
      "type": "value_error.missing"
    }
  ]
})";
    std::wstring title, detail, advice;
    QuickView::AI::AiActionManager::ExtractSemanticError(422, fastapiNested, title, detail, advice);
    EXPECT_NE(title.find(L"HTTP 422"), std::wstring::npos);
    EXPECT_EQ(detail, L"Field required: init_images");
    EXPECT_FALSE(advice.empty());
}

TEST_F(AiActionManagerTest, ProbeLocalSdWebUiModelsIfRunning) {
    HANDLE hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    bool probeSuccess = false;
    std::vector<std::string> fetchedModels;

    QuickView::AI::AiActionManager::Instance().FetchModelsAsync(
        "http://127.0.0.1:7860/", "", QuickView::AI::ApiProtocol::StabilityInpaint,
        [&](bool success, const std::vector<std::string>& models, const std::wstring& /*errMsg*/) {
            probeSuccess = success;
            fetchedModels = models;
            SetEvent(hEvent);
        }
    );

    DWORD waitResult = WaitForSingleObject(hEvent, 4000);
    CloseHandle(hEvent);

    if (waitResult == WAIT_OBJECT_0 && probeSuccess) {
        EXPECT_FALSE(fetchedModels.empty());
        bool foundDreamshaper = false;
        for (const auto& m : fetchedModels) {
            if (m.find("dreamshaper") != std::string::npos || m.find("v1-5") != std::string::npos) {
                foundDreamshaper = true;
                // Verify trailing hash brackets like " [879db523c3]" are stripped cleanly
                EXPECT_EQ(m.find(" ["), std::string::npos);
                break;
            }
        }
        EXPECT_TRUE(foundDreamshaper);
    }
}

TEST_F(AiActionManagerTest, PollSdProgressIfRunning) {
    QuickView::AI::AiActionManager::SdProgressInfo info;
    bool polled = QuickView::AI::AiActionManager::PollSdProgress("http://127.0.0.1:7860/", info);
    // If WebUI Forge daemon is currently running, PollSdProgress must succeed
    if (polled) {
        EXPECT_GE(info.progress, 0.0f);
        EXPECT_LE(info.progress, 1.0f);
    }
}

TEST_F(AiActionManagerTest, TestConnectionAsyncWithLocalSdWebUi) {
    HANDLE hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    bool connSuccess = false;
    int code = 0;

    QuickView::AI::AiActionManager::Instance().TestConnectionAsync(
        "http://127.0.0.1:7860/", "", QuickView::AI::ApiProtocol::StabilityInpaint,
        [&](bool success, int statusCode, int /*latencyMs*/, const std::wstring& /*msg*/) {
            connSuccess = success;
            code = statusCode;
            SetEvent(hEvent);
        }
    );

    DWORD waitResult = WaitForSingleObject(hEvent, 4000);
    CloseHandle(hEvent);

    if (waitResult == WAIT_OBJECT_0 && connSuccess) {
        EXPECT_EQ(code, 200);
    }
}

TEST_F(AiActionManagerTest, FormatAiErrorMessageUnwrapsHttpExceptionDetail) {
    // When FastAPI returns HTTPException 404 with {"error": "HTTPException", "detail": "Init image not found"}
    std::string_view fastapi404 = R"({
  "error": "HTTPException",
  "detail": "Init image not found",
  "body": "",
  "errors": ""
})";
    std::wstring msg = QuickView::AI::AiActionManager::FormatAiErrorMessage(404, fastapi404);
    // Must contain specific detail "Init image not found" rather than generic "HTTPException"
    EXPECT_NE(msg.find(L"Init image not found"), std::wstring::npos);
    EXPECT_EQ(msg.find(L"HTTPException"), std::wstring::npos);
}

TEST_F(AiActionManagerTest, ExtractSemanticErrorHandlesUnhandledMimeType) {
    std::string_view unhandledError = R"({
  "error": {
    "code": 400,
    "message": "Unhandled generated data mime type: image/jpeg",
    "status": "INVALID_ARGUMENT"
  }
})";
    std::wstring title, detail, advice;
    QuickView::AI::AiActionManager::ExtractSemanticError(400, unhandledError, title, detail, advice);
    EXPECT_NE(detail.find(L"Unhandled generated data mime type"), std::wstring::npos);
    EXPECT_NE(advice.find(L"Native REST"), std::wstring::npos);
}

TEST_F(AiActionManagerTest, GeminiNativeProtocolRoundTripPersistence) {
    auto& mgr = QuickView::AI::AiActionManager::Instance();
    auto& profiles = mgr.GetProfiles();
    ASSERT_FALSE(profiles.empty());

    profiles[0].displayName = L"Google Gemini Native";
    profiles[0].baseUrl = "https://generativelanguage.googleapis.com/v1beta/";
    profiles[0].protocol = QuickView::AI::ApiProtocol::GeminiNative;
    profiles[0].defaultModel = "gemini-2.5-flash";

    EXPECT_TRUE(mgr.SaveConfig());
    EXPECT_TRUE(mgr.ReloadConfig());

    const auto& reloaded = mgr.GetProfiles();
    ASSERT_FALSE(reloaded.empty());
    EXPECT_EQ(reloaded[0].protocol, QuickView::AI::ApiProtocol::GeminiNative);
    EXPECT_EQ(reloaded[0].baseUrl, "https://generativelanguage.googleapis.com/v1beta/");
    EXPECT_EQ(reloaded[0].defaultModel, "gemini-2.5-flash");
}

TEST_F(AiActionManagerTest, GeminiNativeEndpointLiveRoutingProbeWithoutKey) {
    // Live probe test against official Google endpoint with legacy URL containing "/openai/"
    // Verifies that:
    // 1. Path sanitization automatically strips "/openai" so it routes to /v1beta/models (no 404)
    // 2. Authentication header uses only "x-goog-api-key" without "Authorization: Bearer" (no 401)
    // 3. Official endpoint responds with HTTP 400 API_KEY_INVALID rather than 401 or 404
    HANDLE hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    int returnedStatusCode = 0;

    QuickView::AI::AiActionManager::Instance().TestConnectionAsync(
        "https://generativelanguage.googleapis.com/v1beta/openai/",
        "AIzaSyTest_DummyKey_For_Routing_Verification",
        QuickView::AI::ApiProtocol::GeminiNative,
        [&](bool /*success*/, int statusCode, int /*latencyMs*/, const std::wstring& /*msg*/) {
            returnedStatusCode = statusCode;
            SetEvent(hEvent);
        }
    );

    DWORD waitResult = WaitForSingleObject(hEvent, 10000);
    CloseHandle(hEvent);

    if (waitResult == WAIT_OBJECT_0 && returnedStatusCode > 0) {
        // If internet connectivity is available, the request must reach GenerativeLanguage API:
        // HTTP 400 means route /v1beta/models was correctly hit and API key was evaluated!
        // It must NOT be 404 (wrong endpoint path) and must NOT be 401 (OAuth Bearer misconfiguration)!
        EXPECT_EQ(returnedStatusCode, 400);
        EXPECT_NE(returnedStatusCode, 401);
        EXPECT_NE(returnedStatusCode, 404);
    }
}




