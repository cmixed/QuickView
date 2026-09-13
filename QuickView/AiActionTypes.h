#pragma once
// ============================================================================
// AiActionTypes.h - Core Data Types for QuickView AI Actions & Model Profiles
// ============================================================================
// Design Principles:
// 1. Separation of Concerns: Model Profiles (Endpoints/Keys) vs Actions (Prompts/Scopes)
// 2. Open Protocol Design: Not hardcoded to specific model versions; supports user-defined model IDs
// 3. No Exceptions: Strict -fno-exceptions compliance; compact memory footprint
// ============================================================================

#include <string>
#include <vector>
#include <cstdint>
#include <string_view>
#include <d2d1.h>

namespace QuickView::AI {

// Supported API Protocol Types
enum class ApiProtocol : uint8_t {
    OpenAiChat = 0,         // OpenAI Chat Completions (Vision Multi-modal, e.g. GPT-4o, Gemini OpenAI-compat, Grok, Qwen-VL)
    OpenAiImagesEdit,       // OpenAI Images Edit (Inpainting multipart/form-data)
    OpenAiImagesGenerate,   // OpenAI Images Generations (DALL-E 3, Flux)
    GeminiNative,           // Google Gemini REST API (v1beta/models/{model}:generateContent)
    StabilityInpaint,       // Stability AI / SD Inpaint API
    ComfyUI,                // Local ComfyUI Webhook
    CustomJsonPost          // Generic HTTP POST JSON
};

// Maximum Resolution Policy for API Payloads
enum class MaxResolution : uint32_t {
    Standard_1K = 1024,     // Standard 1024px (Recommended for SD / Flux / DALL-E)
    FHD_2K      = 2048,     // 2K 2048px (Balanced High-Res)
    Original_4K = 4096,     // Up to 4K 4096px (Native Multi-modal: Gemini 2.0 / GPT-4o)
    NoLimit     = 0         // Send raw resolution (Advanced)
};

// Scope Mode: Full Image vs Selection / Inpainting
enum class ScopeMode : uint8_t {
    Auto = 0,               // Use selection if active, otherwise process full image
    ForceFullImage,         // Always process full image
    CropAndBlend            // Crop selection, inpaint/generate, and alpha-feather back onto base
};

// Model Profile Descriptor (Connection credentials & Endpoint defaults)
struct ModelProfile {
    std::string id;                         // Unique Profile ID (e.g. "gemini_official", "siliconflow")
    std::wstring displayName;               // Display Name in UI (e.g. L"Google Gemini", L"SiliconFlow")
    ApiProtocol protocol = ApiProtocol::OpenAiChat;
    std::string baseUrl;                    // Endpoint Base URL
    std::string encryptedApiKey;            // DPAPI-encrypted Base64 cipher text
    std::string defaultModel;               // User-editable model identifier (e.g. "gemini-2.0-flash", "gpt-4o")
    MaxResolution maxResolution = MaxResolution::Standard_1K;
    int timeoutSeconds = 30;                // Network timeout in seconds
    bool isCustom = true;                   // True if user-created, false if built-in template
    std::vector<std::string> fetchedModels; // Dynamically fetched model list from endpoint
};

// AI Action Descriptor (Prompt template, parameter mappings)
struct ActionDesc {
    std::string id;                         // Unique Action ID
    std::wstring name;                      // Action display name (e.g. L"智能消除与重绘")
    std::string modelProfileId;             // Bound ModelProfile ID (empty = use global default profile)
    std::wstring promptTemplate;            // Prompt template (supports {prompt} macro)
    std::wstring negativePrompt;            // Optional negative prompt
    ScopeMode scopeMode = ScopeMode::Auto;  // Processing target scope
    bool isCollapsed = true;                // Collapsible card state in Settings UI
};

// Execution Context & Result
struct ExecutionResult {
    bool success = false;
    uint32_t httpStatusCode = 0;
    std::wstring errorMessage;              // Human-readable error description with hints
    std::wstring textContent;               // Text response returned by LLM (if any)
    std::string rawResponseBody;            // For debugging / detailed inspection
    std::vector<uint8_t> resultImageData;   // Decoded PNG/JPEG/WebP raw bytes
    uint32_t imageWidth = 0;
    uint32_t imageHeight = 0;
};

} // namespace QuickView::AI
