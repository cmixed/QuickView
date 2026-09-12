#pragma once

#include "ImageLoader.h"
#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <mutex>
#include <condition_variable>

namespace QuickView {

class MappedFile;
struct RawImageFrame;

struct BootPreloadData {
    std::wstring path;
    std::shared_ptr<MappedFile> mmf;
    CImageLoader::ImageHeaderInfo headerInfo{};
    CImageLoader::ImageMetadata metadata{};
    std::shared_ptr<RawImageFrame> rawFrame;
    std::wstring loaderName;
    bool isTitan = false;
    bool decodeSuccess = false;
    std::atomic<bool> headerReady{false};
    std::atomic<bool> frameReady{false};
};

class BootPreloader {
public:
    static BootPreloader& Instance() {
        static BootPreloader s_instance;
        return s_instance;
    }

    // Start asynchronous pre-decoding pipeline immediately upon discovering image path
    void Start(const std::wstring& imagePath);

    // Query or await header information for instant window sizing
    bool GetHeader(CImageLoader::ImageHeaderInfo& outInfo, DWORD timeoutMs = 20);

    // Consume the pre-decoded raw image frame if available (zero-wait hot-path)
    std::shared_ptr<RawImageFrame> TakeFrame(CImageLoader::ImageMetadata* outMeta,
                                             std::wstring* outLoaderName,
                                             std::shared_ptr<MappedFile>* outMmf,
                                             DWORD timeoutMs = 0);

    // True if target is a Titan candidate (>8192 or >50MP)
    bool IsTitan() const;

    // True if decoding is complete (success or failure)
    bool IsFrameReady() const;

    // Cancel or reset state
    void Cancel();

private:
    BootPreloader() = default;
    ~BootPreloader();
    BootPreloader(const BootPreloader&) = delete;
    BootPreloader& operator=(const BootPreloader&) = delete;

    std::shared_ptr<BootPreloadData> m_data;
    std::jthread m_thread;
    std::mutex m_mutex;
    std::condition_variable m_cvHeader;
    std::condition_variable m_cvFrame;
    std::atomic<bool> m_active{false};
};

} // namespace QuickView
