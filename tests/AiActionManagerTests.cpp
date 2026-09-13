#include <gtest/gtest.h>
#include "AiActionManager.h"
#include "OSDState.h"
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
}

TEST_F(AiActionManagerTest, ResetToDefaultsRestoresCleanState) {
    auto& mgr = QuickView::AI::AiActionManager::Instance();
    mgr.ResetToDefaults();

    const auto& profiles = mgr.GetProfiles();
    ASSERT_FALSE(profiles.empty());
    EXPECT_EQ(profiles[0].displayName, L"Google Gemini");
    EXPECT_EQ(profiles[0].defaultModel, "");
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
    EXPECT_NE(msg429.find(L"提示: 请检查账户余额或 API 额度"), std::wstring::npos);
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
    EXPECT_NE(msg503.find(L"切换模型"), std::wstring::npos);
    EXPECT_EQ(msg503.find(L'{'), std::wstring::npos);
    EXPECT_EQ(msg503.find(L'}'), std::wstring::npos);
}

