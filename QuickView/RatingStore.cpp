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

#include "RatingWriter.h"
#include "SupportedExtensions.h"

#include <algorithm>
#include <vector>

namespace {

// A rating sits in the file header, so a small prefix is all that is ever
// read -- the whole point is to stay off the decode pipeline.
constexpr DWORD HEADER_READ_BYTES = 128 * 1024;
constexpr DWORD SIDECAR_READ_BYTES = 16 * 1024;

// Long enough that holding a digit or running 1-3-5 lands one write,
// short enough that the file is up to date by the time the user looks.
constexpr auto WRITE_DEBOUNCE = std::chrono::milliseconds(400);

// An update has to preserve the whole sidecar, so writing reads all of it,
// unlike the prefix that display needs. Anything larger than this is not a
// rating sidecar and is left alone.
constexpr DWORD MAX_SIDECAR_BYTES = 4 * 1024 * 1024;

// How long after writing a file a directory-change notification is assumed to
// be the echo of that write. Kept reasonably small to avoid missing external file ops.
constexpr auto SELF_WRITE_ECHO = std::chrono::milliseconds(600);
std::atomic<int64_t> g_lastSelfWriteTick{ 0 };

LARGE_INTEGER FileSizeOf(const std::wstring& path) {
    LARGE_INTEGER size{};
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) {
        size.HighPart = (LONG)data.nFileSizeHigh;
        size.LowPart = data.nFileSizeLow;
    }
    return size;
}

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
    m_writeWorker = std::thread(&RatingStore::WriteLoop, this);
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

    m_writeCv.notify_all();
    if (m_writeWorker.joinable()) m_writeWorker.join();
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

namespace {

std::optional<int> ReadTiffRatingDirect(const std::wstring& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) return std::nullopt;

    LARGE_INTEGER fileSize{};
    if (!GetFileSizeEx(file, &fileSize) || fileSize.QuadPart < 8) {
        CloseHandle(file);
        return std::nullopt;
    }

    uint8_t header[8];
    DWORD read = 0;
    if (!ReadFile(file, header, 8, &read, nullptr) || read != 8) {
        CloseHandle(file);
        return std::nullopt;
    }

    bool isLE = false;
    if (header[0] == 'I' && header[1] == 'I' && header[2] == 42 && header[3] == 0) {
        isLE = true;
    } else if (header[0] == 'M' && header[1] == 'M' && header[2] == 0 && header[3] == 42) {
        isLE = false;
    } else {
        CloseHandle(file);
        return std::nullopt;
    }

    auto read16 = [isLE](const uint8_t* b) -> uint16_t {
        return isLE ? (uint16_t)(b[0] | (b[1] << 8)) : (uint16_t)((b[0] << 8) | b[1]);
    };
    auto read32 = [isLE](const uint8_t* b) -> uint32_t {
        return isLE ? (uint32_t)(b[0] | (b[1] << 8) | (b[2] << 16) | (b[3] << 24))
                    : (uint32_t)((b[0] << 24) | (b[1] << 16) | (b[2] << 8) | b[3]);
    };

    uint32_t ifdOffset = read32(header + 4);
    if (ifdOffset < 8 || (LONGLONG)ifdOffset + 2 > fileSize.QuadPart) {
        CloseHandle(file);
        return std::nullopt;
    }

    LARGE_INTEGER seekPos{};
    seekPos.QuadPart = ifdOffset;
    if (!SetFilePointerEx(file, seekPos, nullptr, FILE_BEGIN)) {
        CloseHandle(file);
        return std::nullopt;
    }

    uint8_t countBytes[2];
    if (!ReadFile(file, countBytes, 2, &read, nullptr) || read != 2) {
        CloseHandle(file);
        return std::nullopt;
    }

    uint16_t entryCount = read16(countBytes);
    // Sanity limit on IFD entry count to prevent massive allocations on corrupted headers
    if (entryCount == 0 || entryCount > 4096 ||
        (LONGLONG)ifdOffset + 2 + (LONGLONG)entryCount * 12 > fileSize.QuadPart) {
        CloseHandle(file);
        return std::nullopt;
    }

    std::vector<uint8_t> entries(entryCount * 12);
    if (!ReadFile(file, entries.data(), (DWORD)entries.size(), &read, nullptr) || read != entries.size()) {
        CloseHandle(file);
        return std::nullopt;
    }
    CloseHandle(file);

    constexpr uint16_t TAG_SIMPLE_RATING = 0x4746;
    for (uint16_t i = 0; i < entryCount; ++i) {
        const uint8_t* e = entries.data() + i * 12;
        if (read16(e) != TAG_SIMPLE_RATING) continue;

        const uint16_t type = read16(e + 2);
        const uint32_t count = read32(e + 4);
        if (count != 1) return std::nullopt;

        int value = 0;
        switch (type) {
            case 1: value = e[8]; break;
            case 3: value = read16(e + 8); break;
            case 4: value = (int)read32(e + 8); break;
            default: return std::nullopt;
        }
        if (QuickView::Rating::IsValidRating(value)) return value;
    }

    return std::nullopt;
}

} // namespace

std::optional<int> RatingStore::ReadRatingFromFile(const std::wstring& path) {
    const std::wstring_view ext = QuickView::ExtensionOf(path);

    const bool isJpeg = QuickView::ExtEqualsIgnoreCase(ext, L".jpg") ||
                        QuickView::ExtEqualsIgnoreCase(ext, L".jpeg");
    const bool isTiff = QuickView::ExtEqualsIgnoreCase(ext, L".tif") ||
                        QuickView::ExtEqualsIgnoreCase(ext, L".tiff");
    if (!isJpeg && !isTiff) return std::nullopt;

    if (isTiff) {
        // An IFD in a real-world TIFF can reside anywhere, very frequently placed at the
        // end of the file (after all megabytes of pixel strips). Reading only the first
        // 128KB fails for virtually all multi-megabyte TIFFs.
        return ReadTiffRatingDirect(path);
    }

    const std::vector<uint8_t> bytes = ReadFilePrefix(path, HEADER_READ_BYTES);
    if (bytes.empty()) return std::nullopt;
    return QuickView::Rating::ParseJpegRating(bytes);
}

RatingStore::Writability RatingStore::GetWritability(const std::wstring& renderedPath,
                                                    const std::wstring& rawPath) {
    // A RAW is rated through its sidecar, so the RAW itself never has to be
    // writable -- only the folder does, which the write attempt will report.
    if (!rawPath.empty()) return Writability::Writable;

    if (renderedPath.find(L'|') != std::wstring::npos) {
        return Writability::UnsupportedFormat; // entry inside an archive
    }

    const std::wstring_view ext = QuickView::ExtensionOf(renderedPath);
    const bool inFileRatable = QuickView::ExtEqualsIgnoreCase(ext, L".jpg") ||
                               QuickView::ExtEqualsIgnoreCase(ext, L".jpeg") ||
                               QuickView::ExtEqualsIgnoreCase(ext, L".tif") ||
                               QuickView::ExtEqualsIgnoreCase(ext, L".tiff");
    // A standalone RAW still resolves through a sidecar of its own.
    if (!inFileRatable && !QuickView::IsRawPath(renderedPath)) {
        return Writability::UnsupportedFormat;
    }

    const DWORD attrs = GetFileAttributesW(renderedPath.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_READONLY)) {
        return Writability::ReadOnlyFile;
    }
    return Writability::Writable;
}

QuickView::Rating::Resolved RatingStore::ApplyRatingOptimistic(
    ImageID id, int stars, const std::wstring& renderedPath, const std::wstring& rawPath,
    bool isResident) {
    QuickView::Rating::Resolved resolved;
    resolved.stars = stars;
    // A rating set here is the user's, so it is authoritative on both carriers
    // and there is no disagreement left to report.
    resolved.source = rawPath.empty() ? QuickView::Rating::Source::InFile
                                      : QuickView::Rating::Source::Sidecar;
    if (stars == 0) resolved.source = QuickView::Rating::Source::None;

    {
        std::lock_guard<std::mutex> lock(m_cacheMutex);
        m_cache[id] = resolved;
    }

    {
        // Replacing the entry is what makes a burst of keypresses collapse
        // into one write: only the last value survives to reach the disk.
        std::lock_guard<std::mutex> lock(m_writeMutex);
        PendingWrite& pending = m_pendingWrites[id];
        pending.stars = stars;
        pending.renderedPath = renderedPath;
        pending.rawPath = rawPath;
        pending.resident = isResident;
        pending.due = std::chrono::steady_clock::now() + WRITE_DEBOUNCE;
    }
    m_writeCv.notify_one();
    return resolved;
}

void RatingStore::ReleaseResident(const std::wstring& nowResidentPath) {
    bool woke = false;
    {
        std::lock_guard<std::mutex> lock(m_writeMutex);
        for (auto& [id, pending] : m_pendingWrites) {
            if (pending.resident && pending.renderedPath != nowResidentPath) {
                pending.resident = false; // free to rebuild the file now
                pending.due = std::chrono::steady_clock::now();
                woke = true;
            }
        }
    }
    if (woke) m_writeCv.notify_one();
}

void RatingStore::FlushPendingWrites() {
    std::vector<PendingWrite> due;
    {
        std::lock_guard<std::mutex> lock(m_writeMutex);
        for (auto& [id, pending] : m_pendingWrites) {
            pending.resident = false; // nothing is on screen any more
            due.push_back(pending);
        }
        m_pendingWrites.clear();
    }
    for (const auto& write : due) PerformWrite(write);
}

void RatingStore::NotifySelfWrite() {
    g_lastSelfWriteTick.store(
        std::chrono::steady_clock::now().time_since_epoch().count(), std::memory_order_relaxed);
}

bool RatingStore::WasSelfWriteJustNow() {
    const int64_t ticks = g_lastSelfWriteTick.load(std::memory_order_relaxed);
    if (ticks == 0) return false;
    const auto last = std::chrono::steady_clock::time_point(
        std::chrono::steady_clock::duration(ticks));
    return (std::chrono::steady_clock::now() - last) < SELF_WRITE_ECHO;
}

bool RatingStore::WriteSidecar(const std::wstring& ownerPath, int stars, bool allowCreate) {
    const std::wstring sidecarPath = SidecarPathFor(ownerPath);
    if (sidecarPath.empty()) return false;

    // The whole document is read, not just the prefix used for display: an
    // update has to copy through everything it is not changing. A file too
    // large to read whole would come back truncated, and writing that back
    // would destroy the rest of it, so the write is refused instead.
    std::string existing;
    {
        const LARGE_INTEGER size = FileSizeOf(sidecarPath);
        if (size.QuadPart > (LONGLONG)MAX_SIDECAR_BYTES) return false;
        const std::vector<uint8_t> bytes = ReadFilePrefix(sidecarPath, MAX_SIDECAR_BYTES);
        existing.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    }

    std::string updated;
    if (existing.empty()) {
        if (!allowCreate) return true;                          // update-only owner
        if (stars <= QuickView::Rating::MIN_STARS) return true; // nothing to write
        updated = QuickView::Rating::BuildMinimalXmp(stars);
    } else {
        const auto edited = QuickView::Rating::UpdateXmpRating(existing, stars);
        if (!edited) {
            // The document is not shaped as expected. Someone else's develop
            // settings are worth more than this rating, so leave it alone.
            return false;
        }
        if (*edited == existing) return true; // already says what we want
        updated = *edited;
    }

    // Write through a temp file so a failure cannot leave a half-written
    // sidecar where the original was.
    const std::wstring tempPath = sidecarPath + L".qvtmp";
    {
        HANDLE file = CreateFileW(tempPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return false;
        DWORD written = 0;
        const BOOL ok = WriteFile(file, updated.data(), (DWORD)updated.size(), &written, nullptr);
        FlushFileBuffers(file);
        CloseHandle(file);
        if (!ok || written != updated.size()) {
            DeleteFileW(tempPath.c_str());
            return false;
        }
    }

    NotifySelfWrite(); // our own change; the watcher should not rescan for it

    if (existing.empty()) {
        if (!MoveFileExW(tempPath.c_str(), sidecarPath.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            DeleteFileW(tempPath.c_str());
            return false;
        }
        return true;
    }
    if (!ReplaceFileW(sidecarPath.c_str(), tempPath.c_str(), nullptr,
                      REPLACEFILE_IGNORE_MERGE_ERRORS, nullptr, nullptr)) {
        DeleteFileW(tempPath.c_str());
        return false;
    }
    return true;
}

void RatingStore::PerformWrite(const PendingWrite& write) {
    if (write.renderedPath.empty()) return;

    // The sidecar goes first, because it is the side that wins when the two
    // disagree: if the in-file write then fails, what the user sees is still
    // the rating they set, rather than the old one coming back.
    //
    // Which file owns the sidecar mirrors how reading resolves it, and it has
    // to: a sidecar that is read but not written would keep overruling the
    // rating the user just set. A RAW may have one created for it, since that
    // is the only place its rating can live; for anything else an existing
    // sidecar is updated but never brought into being, because a rating
    // belongs inside a JPEG or TIFF.
    const std::wstring sidecarOwner =
        write.rawPath.empty() ? write.renderedPath : write.rawPath;
    const bool ownerIsRaw = QuickView::IsRawPath(sidecarOwner);
    const bool sidecarExists =
        GetFileAttributesW(SidecarPathFor(sidecarOwner).c_str()) != INVALID_FILE_ATTRIBUTES;
    if (ownerIsRaw || sidecarExists) {
        if (!WriteSidecar(sidecarOwner, write.stars, /*allowCreate*/ ownerIsRaw)) {
            return; // refused: leave the in-file half alone as well
        }
    }

    // A standalone RAW has no in-file half to write.
    if (QuickView::IsRawPath(write.renderedPath)) return;

    const auto status = QuickView::Rating::WriteRatingToImage(
        write.renderedPath, write.stars, /*allowTranscode*/ !write.resident);

    if (status == QuickView::Rating::WriteStatus::NeedsTranscode) {
        // The file has no room for an in-place patch and is still on screen.
        // Put it back with no deadline of its own: retrying on a timer would
        // reopen the file every debounce for as long as the photo is shown,
        // and the only thing that can actually unblock it is the photo
        // leaving the screen, which ReleaseResident reports.
        std::lock_guard<std::mutex> lock(m_writeMutex);
        auto& pending = m_pendingWrites[FileNavigator::PathToImageID(write.renderedPath)];
        pending = write;
        pending.resident = true;
        pending.due = std::chrono::steady_clock::time_point::max();
    }
}

void RatingStore::WriteLoop() {
    // WIC is COM, and this thread owns its own apartment: without this every
    // write would fail at CoCreateInstance.
    const HRESULT comInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool ownsCom = SUCCEEDED(comInit);

    struct ComScope {
        bool owns;
        ~ComScope() { if (owns) CoUninitialize(); }
    } comScope{ ownsCom };

    while (true) {
        std::vector<PendingWrite> due;
        {
            std::unique_lock<std::mutex> lock(m_writeMutex);
            if (m_pendingWrites.empty()) {
                m_writeCv.wait(lock, [this] { return !m_running.load() || !m_pendingWrites.empty(); });
            } else {
                // Sleep exactly until the next entry is due rather than on a
                // fixed tick; an entry waiting for its photo to leave the
                // screen has no deadline and must not cause a wakeup at all.
                auto earliest = std::chrono::steady_clock::time_point::max();
                for (const auto& [id, pending] : m_pendingWrites) {
                    earliest = (std::min)(earliest, pending.due);
                }
                if (earliest == std::chrono::steady_clock::time_point::max()) {
                    m_writeCv.wait(lock);
                } else {
                    m_writeCv.wait_until(lock, earliest);
                }
            }
            if (!m_running.load()) break;

            const auto now = std::chrono::steady_clock::now();
            for (auto it = m_pendingWrites.begin(); it != m_pendingWrites.end();) {
                if (it->second.due <= now) {
                    due.push_back(it->second);
                    it = m_pendingWrites.erase(it);
                } else {
                    ++it;
                }
            }
        }
        for (const auto& write : due) PerformWrite(write);
    }

    FlushPendingWrites();
}

std::optional<QuickView::Rating::Resolved> RatingStore::TryGet(ImageID id) const {
    std::lock_guard<std::mutex> lock(m_cacheMutex);
    auto it = m_cache.find(id);
    if (it == m_cache.end()) return std::nullopt;
    return it->second;
}

void RatingStore::QueueRead(ImageID id, const std::wstring& renderedPath,
                            const std::wstring& rawPath) {
    // Deliberately not gated on m_running: the first image can be navigated to
    // before Initialize runs, and such a request must still be honoured once
    // the worker starts rather than being dropped.
    if (renderedPath.empty()) return;
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
