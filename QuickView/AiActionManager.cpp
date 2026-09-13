#include "pch.h"
#include "AiActionManager.h"
#include "yyjson.h"
#include <wincrypt.h>
#include <winhttp.h>
#include <wincodec.h>
#include <shlwapi.h>
#include <thread>
#include <cmath>
#include <algorithm>
#include "EditState.h"
#include "OSDState.h"
#include "AppStrings.h"

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "shlwapi.lib")

extern OSDState g_osd;
extern std::wstring GetCurrentActiveImagePath();

namespace QuickView::AI {

static std::wstring Utf8ToWide(std::string_view utf8Str) {
    if (utf8Str.empty()) return L"";
    int req = MultiByteToWideChar(CP_UTF8, 0, utf8Str.data(), static_cast<int>(utf8Str.size()), nullptr, 0);
    if (req <= 0) return L"";
    std::wstring result(req, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8Str.data(), static_cast<int>(utf8Str.size()), result.data(), req);
    return result;
}

static std::string WideToUtf8(std::wstring_view wideStr) {
    if (wideStr.empty()) return "";
    int req = WideCharToMultiByte(CP_UTF8, 0, wideStr.data(), static_cast<int>(wideStr.size()), nullptr, 0, nullptr, nullptr);
    if (req <= 0) return "";
    std::string result(req, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wideStr.data(), static_cast<int>(wideStr.size()), result.data(), req, nullptr, nullptr);
    return result;
}

static std::string BinaryToBase64(const uint8_t* data, size_t len) {
    if (!data || len == 0) return "";
    DWORD b64Len = 0;
    CryptBinaryToStringA(data, static_cast<DWORD>(len), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &b64Len);
    if (b64Len == 0) return "";
    std::string b64(b64Len, '\0');
    CryptBinaryToStringA(data, static_cast<DWORD>(len), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, b64.data(), &b64Len);
    while (!b64.empty() && (b64.back() == '\0' || b64.back() == '\r' || b64.back() == '\n')) {
        b64.pop_back();
    }
    return b64;
}

static bool LoadAndEncodeActiveImage(const std::wstring& filePath, MaxResolution maxRes, std::string& outBase64, std::string& outMimeType) {
    if (filePath.empty() || !PathFileExistsW(filePath.c_str())) return false;

    std::wstring ext = PathFindExtensionW(filePath.c_str());
    for (auto& c : ext) c = towlower(c);

    HANDLE hFile = CreateFileW(filePath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER fSize{};
    GetFileSizeEx(hFile, &fSize);
    uint32_t limit = (maxRes == MaxResolution::NoLimit) ? 0 : static_cast<uint32_t>(maxRes);

    // Fast path: direct pass-through for existing JPG/PNG/WebP under 4MB if high-res policy
    if (fSize.QuadPart > 0 && fSize.QuadPart <= 4 * 1024 * 1024 &&
        (ext == L".jpg" || ext == L".jpeg" || ext == L".png" || ext == L".webp") && limit >= 2048) {
        std::vector<uint8_t> rawBuf(static_cast<size_t>(fSize.QuadPart));
        DWORD bytesRead = 0;
        if (ReadFile(hFile, rawBuf.data(), static_cast<DWORD>(fSize.QuadPart), &bytesRead, nullptr) && bytesRead == fSize.QuadPart) {
            CloseHandle(hFile);
            outBase64 = BinaryToBase64(rawBuf.data(), rawBuf.size());
            if (ext == L".jpg" || ext == L".jpeg") outMimeType = "image/jpeg";
            else if (ext == L".webp") outMimeType = "image/webp";
            else outMimeType = "image/png";
            return !outBase64.empty();
        }
    }
    CloseHandle(hFile);

    // WIC Robust Decoding and Adaptive Downscaling
    IWICImagingFactory* pFactory = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pFactory)))) {
        return false;
    }

    bool success = false;
    IWICBitmapDecoder* pDecoder = nullptr;
    if (SUCCEEDED(pFactory->CreateDecoderFromFilename(filePath.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &pDecoder))) {
        IWICBitmapFrameDecode* pFrame = nullptr;
        if (SUCCEEDED(pDecoder->GetFrame(0, &pFrame))) {
            UINT origW = 0, origH = 0;
            pFrame->GetSize(&origW, &origH);

            UINT targetW = origW;
            UINT targetH = origH;
            if (limit > 0 && (origW > limit || origH > limit)) {
                if (origW >= origH) {
                    targetW = limit;
                    targetH = (UINT)std::max<UINT>(1, (UINT)std::round((float)origH * (float)limit / (float)origW));
                } else {
                    targetH = limit;
                    targetW = (UINT)std::max<UINT>(1, (UINT)std::round((float)origW * (float)limit / (float)origH));
                }
            }

            IWICBitmapSource* pSource = pFrame;
            IWICBitmapScaler* pScaler = nullptr;
            if (targetW != origW || targetH != origH) {
                if (SUCCEEDED(pFactory->CreateBitmapScaler(&pScaler))) {
                    if (SUCCEEDED(pScaler->Initialize(pFrame, targetW, targetH, WICBitmapInterpolationModeHighQualityCubic))) {
                        pSource = pScaler;
                    }
                }
            }

            IStream* pStream = nullptr;
            if (SUCCEEDED(CreateStreamOnHGlobal(nullptr, TRUE, &pStream))) {
                IWICBitmapEncoder* pEncoder = nullptr;
                if (SUCCEEDED(pFactory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &pEncoder))) {
                    if (SUCCEEDED(pEncoder->Initialize(pStream, WICBitmapEncoderNoCache))) {
                        IWICBitmapFrameEncode* pDstFrame = nullptr;
                        if (SUCCEEDED(pEncoder->CreateNewFrame(&pDstFrame, nullptr))) {
                            pDstFrame->Initialize(nullptr);
                            pDstFrame->SetSize(targetW, targetH);
                            WICPixelFormatGUID pixelFormat = GUID_WICPixelFormatDontCare;
                            pDstFrame->SetPixelFormat(&pixelFormat);
                            pDstFrame->WriteSource(pSource, nullptr);
                            pDstFrame->Commit();
                            pEncoder->Commit();

                            STATSTG stat{};
                            if (SUCCEEDED(pStream->Stat(&stat, STATFLAG_NONAME)) && stat.cbSize.QuadPart > 0) {
                                LARGE_INTEGER liZero{};
                                pStream->Seek(liZero, STREAM_SEEK_SET, nullptr);
                                std::vector<uint8_t> pngBytes(static_cast<size_t>(stat.cbSize.QuadPart));
                                ULONG read = 0;
                                if (SUCCEEDED(pStream->Read(pngBytes.data(), static_cast<ULONG>(pngBytes.size()), &read)) && read == pngBytes.size()) {
                                    outBase64 = BinaryToBase64(pngBytes.data(), pngBytes.size());
                                    outMimeType = "image/png";
                                    success = !outBase64.empty();
                                }
                            }
                            pDstFrame->Release();
                        }
                        pEncoder->Release();
                    }
                }
                pStream->Release();
            }

            if (pScaler) pScaler->Release();
            pFrame->Release();
        }
        pDecoder->Release();
    }
    pFactory->Release();
    return success;
}

std::wstring AiActionManager::FormatAiErrorMessage(DWORD statusCode, std::string_view responseBody) {
    std::wstring mainTitle;
    std::wstring serverDetail;
    std::wstring actionAdvice;

    switch (statusCode) {
    case 429:
        mainTitle = L"API 配额超限或请求过于频繁 (HTTP 429)";
        actionAdvice = L"提示: 请检查账户余额或 API 额度，或稍后重试。";
        break;
    case 503:
        mainTitle = L"服务暂时不可用 (HTTP 503)";
        actionAdvice = L"提示: 服务商模型正处于高峰排队中或代理故障，建议稍后重试或切换模型 (如 gemini-2.5-flash)。";
        break;
    case 404:
        mainTitle = L"服务端未找到该资源或模型 (HTTP 404)";
        actionAdvice = L"提示: 所选模型名称未在该端点上线，请在设置中重新拉取并选择有效模型。";
        break;
    case 401:
        mainTitle = L"API 密钥无效或已过期 (HTTP 401)";
        actionAdvice = L"提示: 请检查密钥是否输入正确或已被服务端吊销。";
        break;
    case 403:
        mainTitle = L"接口访问被拒绝 (HTTP 403)";
        actionAdvice = L"提示: 当前账户无权调用该模型，请检查权限或换用其他模型。";
        break;
    case 413:
        mainTitle = L"图片数据体积超出限制 (HTTP 413)";
        actionAdvice = L"提示: 建议在模型设置中将最大分辨率调为 1024px 或 2048px。";
        break;
    default: {
        wchar_t buf[64] = { 0 };
        swprintf_s(buf, L"服务端响应异常 (HTTP %lu)", statusCode);
        mainTitle = buf;
        actionAdvice = L"提示: 请检查接口地址与网络代理连通性。";
        break;
    }
    }

    if (!responseBody.empty()) {
        yyjson_doc* respDoc = yyjson_read(responseBody.data(), responseBody.size(), 0);
        if (respDoc) {
            yyjson_val* rRoot = yyjson_doc_get_root(respDoc);
            // Unwrap array if returned as array, e.g. [{ "error": ... }]
            if (rRoot && yyjson_is_arr(rRoot) && yyjson_arr_size(rRoot) > 0) {
                rRoot = yyjson_arr_get(rRoot, 0);
            }

            if (rRoot && yyjson_is_obj(rRoot)) {
                const char* mStr = nullptr;
                yyjson_val* vErr = yyjson_obj_get(rRoot, "error");
                if (vErr) {
                    if (yyjson_is_str(vErr)) {
                        mStr = yyjson_get_str(vErr);
                    } else if (yyjson_is_obj(vErr)) {
                        yyjson_val* vMsg = yyjson_obj_get(vErr, "message");
                        if (vMsg && yyjson_is_str(vMsg)) mStr = yyjson_get_str(vMsg);
                        if (!mStr) {
                            yyjson_val* vStat = yyjson_obj_get(vErr, "status");
                            if (vStat && yyjson_is_str(vStat)) mStr = yyjson_get_str(vStat);
                        }
                    }
                }
                if (!mStr) {
                    yyjson_val* vMsg = yyjson_obj_get(rRoot, "message");
                    if (vMsg && yyjson_is_str(vMsg)) mStr = yyjson_get_str(vMsg);
                }
                if (!mStr) {
                    yyjson_val* vDetail = yyjson_obj_get(rRoot, "detail");
                    if (vDetail && yyjson_is_str(vDetail)) mStr = yyjson_get_str(vDetail);
                }

                if (mStr) {
                    serverDetail = Utf8ToWide(mStr);
                }
            }
            yyjson_doc_free(respDoc);
        }

        // If not JSON or failed to extract, check for clean plain text snippet (e.g. gateway error)
        if (serverDetail.empty()) {
            std::string textSnippet;
            for (char c : responseBody) {
                if (c == '\r' || c == '\n' || c == '\t') textSnippet += ' ';
                else if (static_cast<unsigned char>(c) >= 32) textSnippet += c;
                if (textSnippet.size() >= 120) break;
            }
            // Ensure we don't accidentally dump raw unparsed JSON syntax
            if (!textSnippet.empty() && textSnippet.find('{') == std::string::npos && textSnippet.find('[') == std::string::npos) {
                serverDetail = Utf8ToWide(textSnippet);
            }
        }
    }

    // Combine into clean, human-readable formatted message
    std::wstring formatted = L"AI 执行失败: " + mainTitle;
    if (!serverDetail.empty()) {
        while (!serverDetail.empty() && (serverDetail.front() == L' ' || serverDetail.front() == L'\n' || serverDetail.front() == L'\r')) {
            serverDetail.erase(serverDetail.begin());
        }
        while (!serverDetail.empty() && (serverDetail.back() == L' ' || serverDetail.back() == L'\n' || serverDetail.back() == L'\r')) {
            serverDetail.pop_back();
        }
        if (!serverDetail.empty()) {
            formatted += L"\n详情: " + serverDetail;
        }
    }
    if (!actionAdvice.empty()) {
        formatted += L"\n" + actionAdvice;
    }

    return formatted;
}

AiActionManager& AiActionManager::Instance() {
    static AiActionManager instance;
    return instance;
}

AiActionManager::AiActionManager() {
    Init();
}

AiActionManager::~AiActionManager() {
    CancelCurrentTask();
}

bool AiActionManager::Init() {
    if (m_initialized) return true;
    m_initialized = true;
    if (!LoadConfig()) {
        InitDefaultTemplates();
        SaveConfig();
    }
    return true;
}

bool AiActionManager::ReloadConfig() {
    return LoadConfig();
}

void AiActionManager::ResetToDefaults() {
    InitDefaultTemplates();
    SaveConfig();
}

void AiActionManager::InitDefaultTemplates() {
    m_profiles.clear();
    m_actions.clear();

    // Default: Single Profile (Starts with Google Gemini, customizable via dropdown)
    ModelProfile primary;
    primary.id = "primary_profile";
    primary.displayName = L"Google Gemini";
    primary.protocol = ApiProtocol::OpenAiChat;
    primary.baseUrl = "https://generativelanguage.googleapis.com/v1beta/openai/";
    primary.defaultModel = "";
    primary.maxResolution = MaxResolution::Original_4K;
    primary.timeoutSeconds = 45;
    primary.isCustom = false;
    m_profiles.push_back(primary);

    m_defaultProfileId = primary.id;

    // --- Default Actions (Numbered 1..5) ---
    ActionDesc act1;
    act1.id = "action_remove_bg";
    act1.name = L"智能消除背景";
    act1.modelProfileId = ""; // Default
    act1.promptTemplate = L"Remove the background cleanly, make it pure white or transparent while preserving main subject sharp edges";
    act1.scopeMode = ScopeMode::Auto;
    m_actions.push_back(act1);

    ActionDesc act2;
    act2.id = "action_inpaint_fill";
    act2.name = L"局部修补与重绘";
    act2.modelProfileId = "";
    act2.promptTemplate = L"Seamlessly remove the selected object and naturally reconstruct the background with high fidelity";
    act2.scopeMode = ScopeMode::CropAndBlend;
    m_actions.push_back(act2);

    ActionDesc act3;
    act3.id = "action_anime_style";
    act3.name = L"转二次元动漫风格";
    act3.modelProfileId = "";
    act3.promptTemplate = L"Transform this image into a high quality Japanese anime illustration with vibrant colors and cel-shading";
    act3.scopeMode = ScopeMode::ForceFullImage;
    m_actions.push_back(act3);

    ActionDesc act4;
    act4.id = "action_cyberpunk";
    act4.name = L"赛博朋克霓虹风格";
    act4.modelProfileId = "";
    act4.promptTemplate = L"Reimagine this image with cyberpunk aesthetics, glowing neon signs, rainy reflections, and futuristic details";
    act4.scopeMode = ScopeMode::ForceFullImage;
    m_actions.push_back(act4);

    ActionDesc act5;
    act5.id = "action_super_detail";
    act5.name = L"超高清细节增强";
    act5.modelProfileId = "";
    act5.promptTemplate = L"Enhance image sharpness and fine textures, highly detailed, master photography quality";
    act5.scopeMode = ScopeMode::Auto;
    m_actions.push_back(act5);

    m_lastActionId = act1.id;
}

const ModelProfile* AiActionManager::FindProfile(std::string_view profileId) const {
    for (const auto& p : m_profiles) {
        if (p.id == profileId) return &p;
    }
    return nullptr;
}

const ModelProfile* AiActionManager::GetDefaultProfile() const {
    const ModelProfile* p = FindProfile(m_defaultProfileId);
    if (p) return p;
    return m_profiles.empty() ? nullptr : &m_profiles[0];
}

void AiActionManager::SetDefaultProfileId(std::string_view id) {
    m_defaultProfileId = id;
}

void AiActionManager::SetLastActionId(std::string_view id) {
    m_lastActionId = id;
}

std::wstring AiActionManager::GetConfigFilePath() const {
    std::wstring iniPath = GetConfigPath();
    wchar_t szPath[MAX_PATH] = { 0 };
    wcscpy_s(szPath, iniPath.c_str());
    PathRemoveFileSpecW(szPath);
    PathAppendW(szPath, L"ai_actions.json");

    // Seamless migration: If target does not exist yet, check if legacy exeDir version exists
    if (GetFileAttributesW(szPath) == INVALID_FILE_ATTRIBUTES) {
        wchar_t exePath[MAX_PATH] = { 0 };
        if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) > 0) {
            PathRemoveFileSpecW(exePath);
            PathAppendW(exePath, L"ai_actions.json");
            if (_wcsicmp(exePath, szPath) != 0 && GetFileAttributesW(exePath) != INVALID_FILE_ATTRIBUTES) {
                CopyFileW(exePath, szPath, FALSE);
            }
        }
    }
    return szPath;
}

// --- Security: Windows DPAPI En/Decryption ---
std::string AiActionManager::EncryptApiKey(std::string_view plainText) {
    if (plainText.empty()) return "";

    DATA_BLOB dataIn;
    dataIn.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(plainText.data()));
    dataIn.cbData = static_cast<DWORD>(plainText.size());

    DATA_BLOB dataOut{};
    if (!CryptProtectData(&dataIn, L"QuickView_AI_Key", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &dataOut)) {
        return "";
    }

    DWORD base64Len = 0;
    CryptBinaryToStringA(dataOut.pbData, dataOut.cbData, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &base64Len);
    std::string result;
    if (base64Len > 0) {
        result.resize(base64Len);
        CryptBinaryToStringA(dataOut.pbData, dataOut.cbData, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, result.data(), &base64Len);
        while (!result.empty() && (result.back() == '\0' || result.back() == '\r' || result.back() == '\n')) {
            result.pop_back();
        }
    }
    LocalFree(dataOut.pbData);
    return result;
}

std::string AiActionManager::DecryptApiKey(std::string_view cipherBase64) {
    if (cipherBase64.empty()) return "";

    DWORD binaryLen = 0;
    if (!CryptStringToBinaryA(cipherBase64.data(), static_cast<DWORD>(cipherBase64.size()), CRYPT_STRING_BASE64, nullptr, &binaryLen, nullptr, nullptr)) {
        return "";
    }

    std::vector<BYTE> cipherBinary(binaryLen);
    if (!CryptStringToBinaryA(cipherBase64.data(), static_cast<DWORD>(cipherBase64.size()), CRYPT_STRING_BASE64, cipherBinary.data(), &binaryLen, nullptr, nullptr)) {
        return "";
    }

    DATA_BLOB dataIn;
    dataIn.pbData = cipherBinary.data();
    dataIn.cbData = binaryLen;

    DATA_BLOB dataOut{};
    if (!CryptUnprotectData(&dataIn, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &dataOut)) {
        return "";
    }

    std::string plain(reinterpret_cast<char*>(dataOut.pbData), dataOut.cbData);
    LocalFree(dataOut.pbData);
    return plain;
}

// --- Config Persistence via yyjson ---
bool AiActionManager::LoadConfig() {
    std::wstring cfgPath = GetConfigFilePath();
    FILE* fp = nullptr;
    _wfopen_s(&fp, cfgPath.c_str(), L"rb");
    if (!fp) return false;

    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (size <= 0) {
        fclose(fp);
        return false;
    }

    std::vector<char> buf(size + 1, 0);
    fread(buf.data(), 1, size, fp);
    fclose(fp);

    yyjson_doc* doc = yyjson_read(buf.data(), size, 0);
    if (!doc) return false;

    yyjson_val* root = yyjson_doc_get_root(doc);
    if (!root) {
        yyjson_doc_free(doc);
        return false;
    }

    yyjson_val* vDefaultProfile = yyjson_obj_get(root, "default_profile");
    if (vDefaultProfile && yyjson_is_str(vDefaultProfile)) {
        m_defaultProfileId = yyjson_get_str(vDefaultProfile);
    }
    yyjson_val* vLastAction = yyjson_obj_get(root, "last_action");
    if (vLastAction && yyjson_is_str(vLastAction)) {
        m_lastActionId = yyjson_get_str(vLastAction);
    }

    // Load Profiles
    yyjson_val* vProfiles = yyjson_obj_get(root, "profiles");
    if (vProfiles && yyjson_is_arr(vProfiles)) {
        m_profiles.clear();
        size_t idx, maxIdx;
        yyjson_val* pVal;
        yyjson_arr_foreach(vProfiles, idx, maxIdx, pVal) {
            ModelProfile p;
            yyjson_val* id = yyjson_obj_get(pVal, "id");
            if (id) p.id = yyjson_get_str(id);
            yyjson_val* name = yyjson_obj_get(pVal, "name");
            if (name) {
                const char* nStr = yyjson_get_str(name);
                if (nStr) p.displayName = Utf8ToWide(nStr);
            }
            if (p.displayName.empty() || p.displayName[0] < 32) {
                p.displayName = L"Google Gemini";
            }
            yyjson_val* proto = yyjson_obj_get(pVal, "protocol");
            if (proto) p.protocol = static_cast<ApiProtocol>(yyjson_get_int(proto));
            yyjson_val* url = yyjson_obj_get(pVal, "base_url");
            if (url) p.baseUrl = yyjson_get_str(url);
            yyjson_val* key = yyjson_obj_get(pVal, "encrypted_key");
            if (key) p.encryptedApiKey = yyjson_get_str(key);
            yyjson_val* model = yyjson_obj_get(pVal, "default_model");
            if (model) p.defaultModel = yyjson_get_str(model);
            yyjson_val* maxR = yyjson_obj_get(pVal, "max_resolution");
            if (maxR) p.maxResolution = static_cast<MaxResolution>(yyjson_get_uint(maxR));
            yyjson_val* timeout = yyjson_obj_get(pVal, "timeout_seconds");
            if (timeout) p.timeoutSeconds = yyjson_get_int(timeout);
            yyjson_val* custom = yyjson_obj_get(pVal, "is_custom");
            if (custom) p.isCustom = yyjson_get_bool(custom);

            yyjson_val* vFetched = yyjson_obj_get(pVal, "fetched_models");
            if (vFetched && yyjson_is_arr(vFetched)) {
                p.fetchedModels.clear();
                size_t mIdx, mMax;
                yyjson_val* mVal;
                yyjson_arr_foreach(vFetched, mIdx, mMax, mVal) {
                    const char* mStr = yyjson_get_str(mVal);
                    if (mStr) p.fetchedModels.push_back(mStr);
                }
            }

            m_profiles.push_back(p);
        }
    }

    // Load Actions
    yyjson_val* vActions = yyjson_obj_get(root, "actions");
    if (vActions && yyjson_is_arr(vActions)) {
        m_actions.clear();
        size_t idx, maxIdx;
        yyjson_val* aVal;
        yyjson_arr_foreach(vActions, idx, maxIdx, aVal) {
            ActionDesc a;
            yyjson_val* id = yyjson_obj_get(aVal, "id");
            if (id) a.id = yyjson_get_str(id);
            yyjson_val* name = yyjson_obj_get(aVal, "name");
            if (name) {
                const char* nStr = yyjson_get_str(name);
                if (nStr) a.name = Utf8ToWide(nStr);
            }
            yyjson_val* prof = yyjson_obj_get(aVal, "profile_id");
            if (prof) a.modelProfileId = yyjson_get_str(prof);

            yyjson_val* prompt = yyjson_obj_get(aVal, "prompt");
            if (prompt) {
                const char* pStr = yyjson_get_str(prompt);
                if (pStr) a.promptTemplate = Utf8ToWide(pStr);
            }
            yyjson_val* scope = yyjson_obj_get(aVal, "scope_mode");
            if (scope) a.scopeMode = static_cast<ScopeMode>(yyjson_get_int(scope));

            m_actions.push_back(a);
        }
    }

    yyjson_doc_free(doc);
    return true;
}

bool AiActionManager::SaveConfig() {
    yyjson_mut_doc* doc = yyjson_mut_doc_new(nullptr);
    if (!doc) return false;

    yyjson_mut_val* root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    yyjson_mut_obj_add_strcpy(doc, root, "default_profile", m_defaultProfileId.c_str());
    yyjson_mut_obj_add_strcpy(doc, root, "last_action", m_lastActionId.c_str());

    // Serialize Profiles
    yyjson_mut_val* vProfiles = yyjson_mut_arr(doc);
    yyjson_mut_obj_add_val(doc, root, "profiles", vProfiles);
    for (const auto& p : m_profiles) {
        yyjson_mut_val* pVal = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_strcpy(doc, pVal, "id", p.id.c_str());

        std::string utf8Name = WideToUtf8(p.displayName);
        yyjson_mut_obj_add_strcpy(doc, pVal, "name", utf8Name.c_str());

        yyjson_mut_obj_add_int(doc, pVal, "protocol", static_cast<int>(p.protocol));
        yyjson_mut_obj_add_strcpy(doc, pVal, "base_url", p.baseUrl.c_str());
        yyjson_mut_obj_add_strcpy(doc, pVal, "encrypted_key", p.encryptedApiKey.c_str());
        yyjson_mut_obj_add_strcpy(doc, pVal, "default_model", p.defaultModel.c_str());
        yyjson_mut_obj_add_uint(doc, pVal, "max_resolution", static_cast<uint32_t>(p.maxResolution));
        yyjson_mut_obj_add_int(doc, pVal, "timeout_seconds", p.timeoutSeconds);
        yyjson_mut_obj_add_bool(doc, pVal, "is_custom", p.isCustom);

        if (!p.fetchedModels.empty()) {
            yyjson_mut_val* vModels = yyjson_mut_arr(doc);
            yyjson_mut_obj_add_val(doc, pVal, "fetched_models", vModels);
            for (const auto& mName : p.fetchedModels) {
                yyjson_mut_arr_add_strcpy(doc, vModels, mName.c_str());
            }
        }

        yyjson_mut_arr_append(vProfiles, pVal);
    }

    // Serialize Actions
    yyjson_mut_val* vActions = yyjson_mut_arr(doc);
    yyjson_mut_obj_add_val(doc, root, "actions", vActions);
    for (const auto& a : m_actions) {
        yyjson_mut_val* aVal = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_strcpy(doc, aVal, "id", a.id.c_str());

        std::string utf8Name = WideToUtf8(a.name);
        yyjson_mut_obj_add_strcpy(doc, aVal, "name", utf8Name.c_str());

        yyjson_mut_obj_add_strcpy(doc, aVal, "profile_id", a.modelProfileId.c_str());

        std::string utf8Prompt = WideToUtf8(a.promptTemplate);
        yyjson_mut_obj_add_strcpy(doc, aVal, "prompt", utf8Prompt.c_str());

        yyjson_mut_obj_add_int(doc, aVal, "scope_mode", static_cast<int>(a.scopeMode));
        yyjson_mut_arr_append(vActions, aVal);
    }

    size_t jsonLen = 0;
    char* jsonStr = yyjson_mut_write(doc, YYJSON_WRITE_PRETTY, &jsonLen);
    if (!jsonStr) {
        yyjson_mut_doc_free(doc);
        return false;
    }

    std::wstring cfgPath = GetConfigFilePath();

    FILE* fp = nullptr;
    _wfopen_s(&fp, cfgPath.c_str(), L"wb");
    bool ok = false;
    if (fp) {
        fwrite(jsonStr, 1, jsonLen, fp);
        fclose(fp);
        ok = true;
    }
    free(jsonStr);
    yyjson_mut_doc_free(doc);
    return ok;
}

// --- Image Processing & Inpainting Helper ---
bool AiActionManager::PrepareImageBuffer(
    const uint8_t* srcBgra, int srcW, int srcH, int srcStride,
    MaxResolution maxRes, uint32_t stepAlignment,
    std::vector<uint8_t>& outBgra, int& outW, int& outH) {

    if (!srcBgra || srcW <= 0 || srcH <= 0) return false;

    float scale = 1.0f;
    uint32_t limit = static_cast<uint32_t>(maxRes);
    if (limit > 0) {
        int maxDim = (std::max)(srcW, srcH);
        if (maxDim > static_cast<int>(limit)) {
            scale = static_cast<float>(limit) / static_cast<float>(maxDim);
        }
    }

    int targetW = static_cast<int>(std::round(srcW * scale));
    int targetH = static_cast<int>(std::round(srcH * scale));

    if (stepAlignment > 1) {
        targetW = (targetW / stepAlignment) * stepAlignment;
        targetH = (targetH / stepAlignment) * stepAlignment;
        if (targetW <= 0) targetW = stepAlignment;
        if (targetH <= 0) targetH = stepAlignment;
    }

    outW = targetW;
    outH = targetH;
    outBgra.resize(outW * outH * 4);

    // Fast bilinear resample
    float xRatio = static_cast<float>(srcW - 1) / (std::max)(1, targetW - 1);
    float yRatio = static_cast<float>(srcH - 1) / (std::max)(1, targetH - 1);

    for (int y = 0; y < targetH; ++y) {
        float srcY = y * yRatio;
        int yLow = static_cast<int>(srcY);
        int yHigh = (std::min)(yLow + 1, srcH - 1);
        float yWeight = srcY - yLow;

        const uint8_t* rowLow = srcBgra + yLow * srcStride;
        const uint8_t* rowHigh = srcBgra + yHigh * srcStride;
        uint8_t* dstRow = outBgra.data() + y * (outW * 4);

        for (int x = 0; x < targetW; ++x) {
            float srcX = x * xRatio;
            int xLow = static_cast<int>(srcX);
            int xHigh = (std::min)(xLow + 1, srcW - 1);
            float xWeight = srcX - xLow;

            const uint8_t* p00 = rowLow + xLow * 4;
            const uint8_t* p10 = rowLow + xHigh * 4;
            const uint8_t* p01 = rowHigh + xLow * 4;
            const uint8_t* p11 = rowHigh + xHigh * 4;

            for (int c = 0; c < 4; ++c) {
                float val = (1.0f - xWeight) * (1.0f - yWeight) * p00[c] +
                            xWeight * (1.0f - yWeight) * p10[c] +
                            (1.0f - xWeight) * yWeight * p01[c] +
                            xWeight * yWeight * p11[c];
                dstRow[x * 4 + c] = static_cast<uint8_t>(std::clamp(val, 0.0f, 255.0f));
            }
        }
    }
    return true;
}

bool AiActionManager::EncodeToPngMemory(
    const uint8_t* bgra, int width, int height, int stride,
    std::vector<uint8_t>& outPngBytes) {

    if (!bgra || width <= 0 || height <= 0) return false;

    IStream* pStream = nullptr;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &pStream)) || !pStream) {
        return false;
    }

    IWICImagingFactory* pFactory = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pFactory)))) {
        pStream->Release();
        return false;
    }

    IWICBitmapEncoder* pEncoder = nullptr;
    if (FAILED(pFactory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &pEncoder))) {
        pFactory->Release();
        pStream->Release();
        return false;
    }

    pEncoder->Initialize(pStream, WICBitmapEncoderNoCache);
    IWICBitmapFrameEncode* pFrame = nullptr;
    pEncoder->CreateNewFrame(&pFrame, nullptr);
    pFrame->Initialize(nullptr);
    pFrame->SetSize(width, height);

    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    pFrame->SetPixelFormat(&format);
    pFrame->WritePixels(height, stride, stride * height, const_cast<BYTE*>(bgra));
    pFrame->Commit();
    pEncoder->Commit();

    pFrame->Release();
    pEncoder->Release();
    pFactory->Release();

    // Read bytes from stream
    STATSTG stat;
    pStream->Stat(&stat, STATFLAG_NONAME);
    ULONG streamSize = static_cast<ULONG>(stat.cbSize.QuadPart);
    outPngBytes.resize(streamSize);

    LARGE_INTEGER liZero{};
    liZero.QuadPart = 0;
    pStream->Seek(liZero, STREAM_SEEK_SET, nullptr);
    ULONG bytesRead = 0;
    pStream->Read(outPngBytes.data(), streamSize, &bytesRead);
    pStream->Release();

    return bytesRead > 0;
}

void AiActionManager::BlendFeatheredSubImage(
    uint8_t* dstBgra, int dstW, int dstH, int dstStride,
    const uint8_t* srcBgra, int srcW, int srcH, int srcStride,
    int targetX, int targetY, int featherPixels) {

    if (!dstBgra || !srcBgra || srcW <= 0 || srcH <= 0 || dstW <= 0 || dstH <= 0) return;
    featherPixels = (std::max)(1, featherPixels);

    for (int sy = 0; sy < srcH; ++sy) {
        int dy = targetY + sy;
        if (dy < 0 || dy >= dstH) continue;

        uint8_t* dstRow = dstBgra + dy * dstStride;
        const uint8_t* srcRow = srcBgra + sy * srcStride;

        for (int sx = 0; sx < srcW; ++sx) {
            int dx = targetX + sx;
            if (dx < 0 || dx >= dstW) continue;

            // Distance to sub-image boundary
            int distX = (std::min)(sx, srcW - 1 - sx);
            int distY = (std::min)(sy, srcH - 1 - sy);
            int minDist = (std::min)(distX, distY);

            float featherAlpha = 1.0f;
            if (minDist < featherPixels) {
                featherAlpha = static_cast<float>(minDist) / static_cast<float>(featherPixels);
                // Smooth cosine curve
                featherAlpha = 0.5f * (1.0f - std::cos(featherAlpha * 3.14159265f));
            }

            uint8_t* dPixel = dstRow + dx * 4;
            const uint8_t* sPixel = srcRow + sx * 4;

            float srcA = (sPixel[3] / 255.0f) * featherAlpha;
            float invA = 1.0f - srcA;

            dPixel[0] = static_cast<uint8_t>(std::clamp(sPixel[0] * srcA + dPixel[0] * invA, 0.0f, 255.0f));
            dPixel[1] = static_cast<uint8_t>(std::clamp(sPixel[1] * srcA + dPixel[1] * invA, 0.0f, 255.0f));
            dPixel[2] = static_cast<uint8_t>(std::clamp(sPixel[2] * srcA + dPixel[2] * invA, 0.0f, 255.0f));
            dPixel[3] = static_cast<uint8_t>(std::clamp(sPixel[3] * srcA + dPixel[3] * invA, 0.0f, 255.0f));
        }
    }
}

// --- Execution & Task Lifecycle ---
uint64_t AiActionManager::ExecuteAction(
    const ActionDesc& action, HWND hwnd,
    std::function<void(const ExecutionResult&)> onComplete) {

    CancelCurrentTask();

    const ModelProfile* profile = nullptr;
    if (!action.modelProfileId.empty()) {
        profile = FindProfile(action.modelProfileId);
    }
    if (!profile) {
        profile = GetDefaultProfile();
    }
    if (!profile) {
        if (onComplete) {
            ExecutionResult err;
            err.success = false;
            err.errorMessage = L"未找到可用的 AI 模型预设，请检查设置。";
            onComplete(err);
        }
        return 0;
    }

    uint64_t taskId = ++m_currentTaskId;
    m_isRunning.store(true);
    m_lastActionId = action.id;

    ModelProfile profCopy = *profile;
    ActionDesc actCopy = action;

    std::thread([this, taskId, actCopy, profCopy, hwnd, onComplete]() {
        WorkerThread(taskId, actCopy, profCopy, hwnd, onComplete);
    }).detach();

    return taskId;
}

void AiActionManager::CancelCurrentTask() {
    m_currentTaskId.fetch_add(1);
    m_isRunning.store(false);

    std::lock_guard<std::mutex> lock(m_taskMutex);
    if (m_activeRequest) {
        WinHttpCloseHandle(m_activeRequest);
        m_activeRequest = nullptr;
    }
    if (m_activeConnect) {
        WinHttpCloseHandle(m_activeConnect);
        m_activeConnect = nullptr;
    }
    if (m_activeSession) {
        WinHttpCloseHandle(m_activeSession);
        m_activeSession = nullptr;
    }
}

void AiActionManager::WorkerThread(
    uint64_t taskId, ActionDesc action, ModelProfile profile,
    HWND /*hwnd*/, std::function<void(const ExecutionResult&)> callback) {

    ExecutionResult result;
    result.success = false;

    // Check cancellation
    if (taskId != m_currentTaskId.load()) {
        m_isRunning.store(false);
        return;
    }

    bool isLocal = (profile.protocol == ApiProtocol::ComfyUI ||
                    profile.protocol == ApiProtocol::StabilityInpaint ||
                    profile.baseUrl.find("127.0.0.1") != std::string::npos ||
                    profile.baseUrl.find("localhost") != std::string::npos);

    std::string apiKey = DecryptApiKey(profile.encryptedApiKey);
    if (!isLocal && apiKey.empty()) {
        result.errorMessage = L"API 密钥为空，请先在【设置 -> AI 动作 -> 模型服务商】中配置该服务商的 API 密钥。";
        m_isRunning.store(false);
        if (callback) callback(result);
        return;
    }

    // Determine target model
    std::string targetModel = profile.defaultModel;
    if (targetModel.empty()) {
        result.errorMessage = L"未配置模型标识，请先在【设置 -> AI 动作 -> 模型服务商】中拉取或输入有效模型。";
        m_isRunning.store(false);
        if (callback) callback(result);
        return;
    }

    // Parse URL
    std::wstring wUrl;
    int wlen = MultiByteToWideChar(CP_UTF8, 0, profile.baseUrl.c_str(), -1, nullptr, 0);
    if (wlen > 0) {
        wUrl.resize(wlen - 1);
        MultiByteToWideChar(CP_UTF8, 0, profile.baseUrl.c_str(), -1, wUrl.data(), wlen);
    }

    URL_COMPONENTS urlComp{};
    urlComp.dwStructSize = sizeof(urlComp);
    wchar_t hostName[256] = { 0 };
    wchar_t urlPath[1024] = { 0 };
    urlComp.lpszHostName = hostName;
    urlComp.dwHostNameLength = 256;
    urlComp.lpszUrlPath = urlPath;
    urlComp.dwUrlPathLength = 1024;

    if (!WinHttpCrackUrl(wUrl.c_str(), static_cast<DWORD>(wUrl.size()), 0, &urlComp)) {
        result.errorMessage = L"无效的 Base URL 格式。";
        m_isRunning.store(false);
        if (callback) callback(result);
        return;
    }

    // Append standard endpoint path if necessary
    std::wstring fullPath = urlPath;
    if (fullPath.empty() || fullPath.back() != L'/') fullPath += L'/';

    if (profile.protocol == ApiProtocol::OpenAiChat) {
        if (fullPath.find(L"chat/completions") == std::wstring::npos) {
            fullPath += L"chat/completions";
        }
    } else if (profile.protocol == ApiProtocol::OpenAiImagesGenerate) {
        if (fullPath.find(L"images/generations") == std::wstring::npos) {
            fullPath += L"images/generations";
        }
    } else if (profile.protocol == ApiProtocol::OpenAiImagesEdit) {
        if (fullPath.find(L"images/edits") == std::wstring::npos) {
            fullPath += L"images/edits";
        }
    }

    // Initialize WinHTTP Session
    {
        std::lock_guard<std::mutex> lock(m_taskMutex);
        m_activeSession = WinHttpOpen(L"QuickView-AI/2.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!m_activeSession) {
            result.errorMessage = L"初始化 WinHTTP 失败。";
            m_isRunning.store(false);
            if (callback) callback(result);
            return;
        }

        DWORD timeoutMs = profile.timeoutSeconds * 1000;
        WinHttpSetTimeouts(m_activeSession, timeoutMs, timeoutMs, timeoutMs, timeoutMs);

        m_activeConnect = WinHttpConnect(m_activeSession, hostName, urlComp.nPort, 0);
        if (!m_activeConnect) {
            result.errorMessage = L"连接服务器失败，请检查网络或 Endpoint。";
            m_isRunning.store(false);
            if (callback) callback(result);
            return;
        }

        DWORD flags = (urlComp.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
        m_activeRequest = WinHttpOpenRequest(m_activeConnect, L"POST", fullPath.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
        if (!m_activeRequest) {
            result.errorMessage = L"创建 HTTP 请求失败。";
            m_isRunning.store(false);
            if (callback) callback(result);
            return;
        }
    }

    // Load and encode active image (if any)
    std::string imageBase64;
    std::string imageMimeType;
    bool hasImage = LoadAndEncodeActiveImage(GetCurrentActiveImagePath(), profile.maxResolution, imageBase64, imageMimeType);

    // Build JSON Payload via yyjson
    yyjson_mut_doc* doc = yyjson_mut_doc_new(nullptr);
    yyjson_mut_val* root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    yyjson_mut_obj_add_str(doc, root, "model", targetModel.c_str());

    // Prompt UTF-8
    std::string utf8Prompt = WideToUtf8(action.promptTemplate);

    if (profile.protocol == ApiProtocol::OpenAiImagesGenerate) {
        yyjson_mut_obj_add_str(doc, root, "prompt", utf8Prompt.c_str());
        yyjson_mut_obj_add_int(doc, root, "n", 1);
        yyjson_mut_obj_add_str(doc, root, "response_format", "b64_json");
        yyjson_mut_obj_add_str(doc, root, "size", "1024x1024");
    } else {
        // Chat Completions Format (Multi-modal Vision support)
        yyjson_mut_val* msgs = yyjson_mut_arr(doc);
        yyjson_mut_obj_add_val(doc, root, "messages", msgs);

        yyjson_mut_val* userMsg = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_str(doc, userMsg, "role", "user");

        if (hasImage) {
            // Standard OpenAI Vision format: array with text + image_url
            yyjson_mut_val* contentArr = yyjson_mut_arr(doc);
            yyjson_mut_obj_add_val(doc, userMsg, "content", contentArr);

            // 1. Text Prompt
            yyjson_mut_val* textObj = yyjson_mut_obj(doc);
            yyjson_mut_obj_add_str(doc, textObj, "type", "text");
            yyjson_mut_obj_add_str(doc, textObj, "text", utf8Prompt.c_str());
            yyjson_mut_arr_append(contentArr, textObj);

            // 2. Image URL (Data URL Base64)
            yyjson_mut_val* imgObj = yyjson_mut_obj(doc);
            yyjson_mut_obj_add_str(doc, imgObj, "type", "image_url");
            yyjson_mut_val* urlChild = yyjson_mut_obj(doc);
            std::string dataUrl = "data:" + imageMimeType + ";base64," + imageBase64;
            yyjson_mut_obj_add_str(doc, urlChild, "url", dataUrl.c_str());
            yyjson_mut_obj_add_val(doc, imgObj, "image_url", urlChild);
            yyjson_mut_arr_append(contentArr, imgObj);
        } else {
            // Pure text prompt
            yyjson_mut_obj_add_str(doc, userMsg, "content", utf8Prompt.c_str());
        }
        yyjson_mut_arr_append(msgs, userMsg);
    }

    size_t payloadLen = 0;
    char* payloadStr = yyjson_mut_write(doc, 0, &payloadLen);
    yyjson_mut_doc_free(doc);

    // Build Headers
    std::wstring headers = L"Content-Type: application/json\r\n";
    if (!apiKey.empty()) {
        std::wstring wKey;
        int kLen = MultiByteToWideChar(CP_UTF8, 0, apiKey.c_str(), -1, nullptr, 0);
        if (kLen > 0) {
            wKey.resize(kLen - 1);
            MultiByteToWideChar(CP_UTF8, 0, apiKey.c_str(), -1, wKey.data(), kLen);
        }
        headers += L"Authorization: Bearer " + wKey + L"\r\n";
    }

    // Send Request
    BOOL bSend = WinHttpSendRequest(m_activeRequest, headers.c_str(), static_cast<DWORD>(headers.size()),
                                    payloadStr, static_cast<DWORD>(payloadLen), static_cast<DWORD>(payloadLen), 0);
    free(payloadStr);

    if (!bSend || !WinHttpReceiveResponse(m_activeRequest, nullptr)) {
        DWORD dwErr = GetLastError();
        wchar_t errBuf[128] = { 0 };
        swprintf_s(errBuf, L"网络请求发送失败 (Error: %lu)。请检查代理设置或网络状态。", dwErr);
        result.errorMessage = errBuf;
        m_isRunning.store(false);
        if (callback) callback(result);
        return;
    }

    // Query Status Code
    DWORD statusCode = 0;
    DWORD scSize = sizeof(statusCode);
    WinHttpQueryHeaders(m_activeRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &scSize, WINHTTP_NO_HEADER_INDEX);
    result.httpStatusCode = statusCode;

    // Read Response Body
    std::string responseBody;
    DWORD bytesAvailable = 0;
    while (WinHttpQueryDataAvailable(m_activeRequest, &bytesAvailable) && bytesAvailable > 0) {
        if (taskId != m_currentTaskId.load()) {
            m_isRunning.store(false);
            return; // Interrupted
        }
        std::vector<char> tempBuf(bytesAvailable);
        DWORD bytesRead = 0;
        if (WinHttpReadData(m_activeRequest, tempBuf.data(), bytesAvailable, &bytesRead) && bytesRead > 0) {
            responseBody.append(tempBuf.data(), bytesRead);
        }
    }
    result.rawResponseBody = responseBody;

    // Handle Error Status Code
    if (statusCode != 200) {
        result.errorMessage = FormatAiErrorMessage(statusCode, responseBody);
        m_isRunning.store(false);
        if (callback) callback(result);
        return;
    }

    // Success (HTTP 200): Parse Image Data (Base64 or URL) or Text Content
    yyjson_doc* respDoc = yyjson_read(responseBody.data(), responseBody.size(), 0);
    if (respDoc) {
        yyjson_val* rRoot = yyjson_doc_get_root(respDoc);
        if (rRoot) {
            // 1. Try standard OpenAI image generation structure: data[0].b64_json
            yyjson_val* vData = yyjson_obj_get(rRoot, "data");
            if (vData && yyjson_is_arr(vData) && yyjson_arr_size(vData) > 0) {
                yyjson_val* first = yyjson_arr_get(vData, 0);
                yyjson_val* b64 = yyjson_obj_get(first, "b64_json");
                if (b64 && yyjson_is_str(b64)) {
                    const char* b64Str = yyjson_get_str(b64);
                    DWORD binLen = 0;
                    if (CryptStringToBinaryA(b64Str, static_cast<DWORD>(strlen(b64Str)), CRYPT_STRING_BASE64, nullptr, &binLen, nullptr, nullptr)) {
                        result.resultImageData.resize(binLen);
                        CryptStringToBinaryA(b64Str, static_cast<DWORD>(strlen(b64Str)), CRYPT_STRING_BASE64, result.resultImageData.data(), &binLen, nullptr, nullptr);
                        result.success = true;
                    }
                }
            }

            // 2. Try Chat Completions structure: choices[0].message.content
            if (!result.success) {
                yyjson_val* vChoices = yyjson_obj_get(rRoot, "choices");
                if (vChoices && yyjson_is_arr(vChoices) && yyjson_arr_size(vChoices) > 0) {
                    yyjson_val* firstChoice = yyjson_arr_get(vChoices, 0);
                    yyjson_val* msgObj = yyjson_obj_get(firstChoice, "message");
                    if (msgObj) {
                        yyjson_val* cVal = yyjson_obj_get(msgObj, "content");
                        if (cVal && yyjson_is_str(cVal)) {
                            const char* cStr = yyjson_get_str(cVal);
                            std::string contentStr = cStr ? cStr : "";

                            // Look for embedded Base64 image
                            size_t b64Pos = contentStr.find(";base64,");
                            if (b64Pos != std::string::npos) {
                                size_t startPos = b64Pos + 8;
                                size_t endPos = contentStr.find_first_of(")\"'\r\n \t", startPos);
                                std::string subB64 = (endPos == std::string::npos) ? contentStr.substr(startPos) : contentStr.substr(startPos, endPos - startPos);

                                DWORD binLen = 0;
                                if (CryptStringToBinaryA(subB64.c_str(), static_cast<DWORD>(subB64.size()), CRYPT_STRING_BASE64, nullptr, &binLen, nullptr, nullptr)) {
                                    result.resultImageData.resize(binLen);
                                    CryptStringToBinaryA(subB64.c_str(), static_cast<DWORD>(subB64.size()), CRYPT_STRING_BASE64, result.resultImageData.data(), &binLen, nullptr, nullptr);
                                    result.success = true;
                                }
                            }

                            // If not an image, preserve full text response
                            if (!result.success && !contentStr.empty()) {
                                result.textContent = Utf8ToWide(contentStr);
                                result.success = true;
                            }
                        }
                    }
                }
            }
        }
        yyjson_doc_free(respDoc);
    }

    if (!result.success) {
        result.errorMessage = L"API 响应成功，但未解析到有效的图片或文字数据。";
    }

    m_isRunning.store(false);
    if (callback && taskId == m_currentTaskId.load()) {
        callback(result);
    }
}

void AiActionManager::FetchModelsAsync(
    std::string baseUrl,
    std::string apiKey,
    ApiProtocol /*protocol*/,
    std::function<void(bool success, const std::vector<std::string>& models, const std::wstring& errorMsg)> onComplete)
{
    std::thread([baseUrl = std::move(baseUrl), apiKey = std::move(apiKey), onComplete = std::move(onComplete)]() {
        std::vector<std::string> models;

        if (baseUrl.empty()) {
            if (onComplete) onComplete(false, {}, L"接口地址 (Base URL) 为空");
            return;
        }

        // Parse URL
        URL_COMPONENTS urlComp{};
        urlComp.dwStructSize = sizeof(urlComp);
        urlComp.dwSchemeLength = static_cast<DWORD>(-1);
        urlComp.dwHostNameLength = static_cast<DWORD>(-1);
        urlComp.dwUrlPathLength = static_cast<DWORD>(-1);

        std::wstring wUrl(baseUrl.begin(), baseUrl.end());
        if (!WinHttpCrackUrl(wUrl.c_str(), static_cast<DWORD>(wUrl.length()), 0, &urlComp)) {
            if (onComplete) onComplete(false, {}, L"无效的 URL 格式");
            return;
        }

        std::wstring host(urlComp.lpszHostName, urlComp.dwHostNameLength);
        std::wstring path(urlComp.lpszUrlPath, urlComp.dwUrlPathLength);
        if (path.empty() || path.back() != L'/') path += L'/';
        path += L"models";

        HINTERNET hSession = WinHttpOpen(L"QuickView-AI-Fetch/2.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!hSession) {
            if (onComplete) onComplete(false, {}, L"初始化网络会话失败");
            return;
        }

        HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(), urlComp.nPort, 0);
        if (!hConnect) {
            WinHttpCloseHandle(hSession);
            if (onComplete) onComplete(false, {}, L"连接服务器失败");
            return;
        }

        DWORD flags = (urlComp.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
        HINTERNET hReq = WinHttpOpenRequest(hConnect, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
        if (!hReq) {
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
            if (onComplete) onComplete(false, {}, L"创建 HTTP 请求失败");
            return;
        }

        // 15s timeout
        WinHttpSetTimeouts(hReq, 5000, 5000, 15000, 15000);

        std::wstring headers;
        if (!apiKey.empty()) {
            headers = L"Authorization: Bearer " + std::wstring(apiKey.begin(), apiKey.end()) + L"\r\n";
        }

        BOOL sent = WinHttpSendRequest(hReq,
            headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
            static_cast<DWORD>(headers.length()),
            WINHTTP_NO_REQUEST_DATA, 0, 0, 0);

        if (!sent || !WinHttpReceiveResponse(hReq, nullptr)) {
            WinHttpCloseHandle(hReq);
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
            if (onComplete) onComplete(false, {}, L"网络请求超时或无响应");
            return;
        }

        DWORD statusCode = 0;
        DWORD dwSize = sizeof(statusCode);
        WinHttpQueryHeaders(hReq, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &dwSize, WINHTTP_NO_HEADER_INDEX);

        std::string respBody;
        DWORD bytesAvailable = 0;
        while (WinHttpQueryDataAvailable(hReq, &bytesAvailable) && bytesAvailable > 0) {
            std::vector<char> buf(bytesAvailable);
            DWORD bytesRead = 0;
            if (WinHttpReadData(hReq, buf.data(), bytesAvailable, &bytesRead) && bytesRead > 0) {
                respBody.append(buf.data(), bytesRead);
            }
        }

        WinHttpCloseHandle(hReq);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);

        if (statusCode != 200) {
            if (onComplete) onComplete(false, {}, L"服务商返回错误 (HTTP " + std::to_wstring(statusCode) + L")");
            return;
        }

        // Parse JSON via yyjson
        yyjson_doc* doc = yyjson_read(respBody.c_str(), respBody.size(), 0);
        if (!doc) {
            if (onComplete) onComplete(false, {}, L"无法解析服务商返回的 JSON 数据");
            return;
        }

        yyjson_val* root = yyjson_doc_get_root(doc);
        if (root) {
            yyjson_val* dataArr = yyjson_obj_get(root, "data");
            if (!dataArr || !yyjson_is_arr(dataArr)) {
                dataArr = yyjson_obj_get(root, "models");
            }

            if (dataArr && yyjson_is_arr(dataArr)) {
                size_t idx, max;
                yyjson_val* item;
                yyjson_arr_foreach(dataArr, idx, max, item) {
                    yyjson_val* idVal = yyjson_obj_get(item, "id");
                    if (!idVal) idVal = yyjson_obj_get(item, "name");
                    if (idVal && yyjson_is_str(idVal)) {
                        std::string idStr = yyjson_get_str(idVal);
                        if (idStr.rfind("models/", 0) == 0) {
                            idStr = idStr.substr(7);
                        }
                        models.push_back(std::move(idStr));
                    }
                }
            }
        }
        yyjson_doc_free(doc);

        std::sort(models.begin(), models.end());
        models.erase(std::unique(models.begin(), models.end()), models.end());

        if (models.empty()) {
            if (onComplete) onComplete(false, {}, L"接口返回成功，但未解析到可用模型列表");
        } else {
            if (onComplete) onComplete(true, models, L"");
        }
    }).detach();
}

void AiActionManager::TestConnectionAsync(
    std::string baseUrl,
    std::string apiKey,
    ApiProtocol /*protocol*/,
    std::function<void(bool success, int statusCode, int latencyMs, const std::wstring& message)> onComplete)
{
    std::thread([baseUrl = std::move(baseUrl), apiKey = std::move(apiKey), onComplete = std::move(onComplete)]() {
        if (baseUrl.empty()) {
            if (onComplete) onComplete(false, 0, 0, L"接口地址 (Base URL) 为空");
            return;
        }

        URL_COMPONENTS urlComp{};
        urlComp.dwStructSize = sizeof(urlComp);
        urlComp.dwSchemeLength = static_cast<DWORD>(-1);
        urlComp.dwHostNameLength = static_cast<DWORD>(-1);
        urlComp.dwUrlPathLength = static_cast<DWORD>(-1);

        std::wstring wUrl(baseUrl.begin(), baseUrl.end());
        if (!WinHttpCrackUrl(wUrl.c_str(), static_cast<DWORD>(wUrl.length()), 0, &urlComp)) {
            if (onComplete) onComplete(false, 0, 0, L"无效的 URL 格式");
            return;
        }

        std::wstring host(urlComp.lpszHostName, urlComp.dwHostNameLength);
        std::wstring path(urlComp.lpszUrlPath, urlComp.dwUrlPathLength);
        if (path.empty() || path.back() != L'/') path += L'/';
        path += L"models";

        auto startTime = std::chrono::steady_clock::now();

        HINTERNET hSession = WinHttpOpen(L"QuickView-AI-Probe/2.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!hSession) {
            if (onComplete) onComplete(false, 0, 0, L"初始化网络会话失败");
            return;
        }

        HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(), urlComp.nPort, 0);
        if (!hConnect) {
            WinHttpCloseHandle(hSession);
            if (onComplete) onComplete(false, 0, 0, L"无法连接到目标主机");
            return;
        }

        DWORD flags = (urlComp.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
        HINTERNET hReq = WinHttpOpenRequest(hConnect, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
        if (!hReq) {
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
            if (onComplete) onComplete(false, 0, 0, L"创建 HTTP 请求句柄失败");
            return;
        }

        // 10s probe timeout
        WinHttpSetTimeouts(hReq, 3000, 4000, 10000, 10000);

        std::wstring headers;
        if (!apiKey.empty()) {
            headers = L"Authorization: Bearer " + std::wstring(apiKey.begin(), apiKey.end()) + L"\r\n";
        }

        BOOL sent = WinHttpSendRequest(hReq,
            headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
            static_cast<DWORD>(headers.length()),
            WINHTTP_NO_REQUEST_DATA, 0, 0, 0);

        if (!sent || !WinHttpReceiveResponse(hReq, nullptr)) {
            WinHttpCloseHandle(hReq);
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
            if (onComplete) onComplete(false, 0, 0, L"网络请求超时或主机无响应");
            return;
        }

        auto endTime = std::chrono::steady_clock::now();
        int latencyMs = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime).count());

        DWORD statusCode = 0;
        DWORD dwSize = sizeof(statusCode);
        WinHttpQueryHeaders(hReq, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &dwSize, WINHTTP_NO_HEADER_INDEX);

        WinHttpCloseHandle(hReq);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);

        if (statusCode >= 200 && statusCode < 300) {
            if (onComplete) onComplete(true, static_cast<int>(statusCode), latencyMs, L"连接正常且凭据鉴权通过");
        } else if (statusCode == 401 || statusCode == 403) {
            if (onComplete) onComplete(false, static_cast<int>(statusCode), latencyMs, L"API 密钥无效或未授权 (HTTP " + std::to_wstring(statusCode) + L")");
        } else {
            if (onComplete) onComplete(false, static_cast<int>(statusCode), latencyMs, L"服务器返回 HTTP " + std::to_wstring(statusCode));
        }
    }).detach();
}

} // namespace QuickView::AI
