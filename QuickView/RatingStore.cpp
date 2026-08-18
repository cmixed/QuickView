/*
 * QuickView Star Ratings - isolated rating cache and background reader
 * Copyright (C) 2026-Present QuickView Contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "RatingStore.h"

#include "SupportedExtensions.h"

#include <vector>

namespace {

// A rating sits in the file header, so a small prefix is all that is ever
// read -- the whole point is to stay off the decode pipeline.
constexpr DWORD HEADER_READ_BYTES = 128 * 1024;
constexpr DWORD SIDECAR_READ_BYTES = 16 * 1024;

// Wide-char file API throughout: a path round-tripped through a narrow code
// page fails to open on non-ASCII names.
std::vector<uint8_t> ReadFilePrefix(const std::wstring& path, DWORD maxBytes) {
    std::vector<uint8_t> buffer;
    if (path.empty()) return buffer;

    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) return buffer;

    LARGE_INTEGER size{};
    if (GetFileSizeEx(file, &size) && size.QuadPart > 0) {
        const DWORD toRead = (size.QuadPart < (LONGLONG)maxBytes)
                                 ? (DWORD)size.QuadPart
                                 : maxBytes;
        buffer.resize(toRead);
        DWORD read = 0;
        if (!ReadFile(file, buffer.data(), toRead, &read, nullptr)) {
            buffer.clear();
        } else {
            buffer.resize(read);
        }
    }
    CloseHandle(file);
    return buffer;
}

} // namespace

RatingStore::~RatingStore() {
    Shutdown();
}

void RatingStore::Initialize(HWND hwnd) {
    if (m_running.load()) return;
    m_hwnd = hwnd;
    m_running = true;
    m_worker = std::thread(&RatingStore::WorkerLoop, this);
}

void RatingStore::Shutdown() {
    if (!m_running.exchange(false)) return;
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_queue.clear();
        m_pending.clear();
    }
    m_cv.notify_all();
    if (m_worker.joinable()) m_worker.join();
}

std::wstring RatingStore::SidecarPathFor(const std::wstring& path) {
    const std::wstring_view ext = QuickView::ExtensionOf(path);
    if (ext.empty()) return std::wstring();
    return path.substr(0, path.size() - ext.size()) + L".xmp";
}

std::optional<int> RatingStore::ReadRatingFromSidecar(const std::wstring& sidecarPath) {
    const std::vector<uint8_t> bytes = ReadFilePrefix(sidecarPath, SIDECAR_READ_BYTES);
    if (bytes.empty()) return std::nullopt;
    return QuickView::Rating::ParseXmpRating(
        std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
}

std::optional<int> RatingStore::ReadRatingFromFile(const std::wstring& path) {
    const std::wstring_view ext = QuickView::ExtensionOf(path);

    // Only formats that actually carry an in-file rating are opened at all.
    const bool isJpeg = QuickView::ExtEqualsIgnoreCase(ext, L".jpg") ||
                        QuickView::ExtEqualsIgnoreCase(ext, L".jpeg");
    const bool isTiff = QuickView::ExtEqualsIgnoreCase(ext, L".tif") ||
                        QuickView::ExtEqualsIgnoreCase(ext, L".tiff");
    if (!isJpeg && !isTiff) return std::nullopt;

    const std::vector<uint8_t> bytes = ReadFilePrefix(path, HEADER_READ_BYTES);
    if (bytes.empty()) return std::nullopt;

    return isJpeg ? QuickView::Rating::ParseJpegRating(bytes)
                  : QuickView::Rating::ParseTiffRating(bytes);
}

std::optional<QuickView::Rating::Resolved> RatingStore::TryGet(ImageID id) const {
    std::lock_guard<std::mutex> lock(m_cacheMutex);
    auto it = m_cache.find(id);
    if (it == m_cache.end()) return std::nullopt;
    return it->second;
}

void RatingStore::QueueRead(ImageID id, const std::wstring& renderedPath,
                            const std::wstring& rawPath) {
    if (!m_running.load() || renderedPath.empty()) return;
    {
        std::lock_guard<std::mutex> cacheLock(m_cacheMutex);
        if (m_cache.find(id) != m_cache.end()) return; // already known
    }
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        if (!m_pending.insert(id).second) return;      // already queued
        m_queue.push_back(Task{ id, renderedPath, rawPath, m_generation.load() });
    }
    m_cv.notify_one();
}

void RatingStore::Clear() {
    ++m_generation; // invalidates results still in flight
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_queue.clear();
        m_pending.clear();
    }
    std::lock_guard<std::mutex> cacheLock(m_cacheMutex);
    m_cache.clear();
}

void RatingStore::WorkerLoop() {
    while (true) {
        Task task;
        {
            std::unique_lock<std::mutex> lock(m_queueMutex);
            m_cv.wait(lock, [this] { return !m_running.load() || !m_queue.empty(); });
            if (!m_running.load()) return;
            task = std::move(m_queue.front());
            m_queue.pop_front();
        }

        // A rating can live in the file itself (JPEG/TIFF) and in the sidecar
        // of the paired RAW. A standalone RAW has no in-file rating, so only
        // its own sidecar is probed.
        const std::optional<int> inFile = ReadRatingFromFile(task.renderedPath);
        const std::wstring sidecarOwner = task.rawPath.empty() ? task.renderedPath : task.rawPath;
        const std::optional<int> sidecar = ReadRatingFromSidecar(SidecarPathFor(sidecarOwner));

        const auto resolved = QuickView::Rating::ResolvePairRating(inFile, sidecar);

        if (task.generation != m_generation.load()) continue; // folder moved on

        {
            std::lock_guard<std::mutex> cacheLock(m_cacheMutex);
            m_cache[task.id] = resolved;
        }
        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            m_pending.erase(task.id);
        }
        if (m_hwnd) {
            PostMessageW(m_hwnd, WM_RATING_READY, (WPARAM)task.id, 0);
        }
    }
}
