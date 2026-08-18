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

#pragma once

#include "pch.h"
#include "FileNavigator.h"   // ImageID
#include "RatingMetadata.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>

// Posted (wParam = ImageID) once a background read has filled the cache, so
// the UI can repaint just the affected item.
#define WM_RATING_READY (WM_APP + 103)

// Ratings live entirely outside the decode pipeline: nothing here may be
// called synchronously from CImageLoader::ReadMetadata or the thumbnail path.
// The UI asks the cache (TryGet, never touches disk) and queues a background
// read when the answer is not there yet.
class RatingStore {
public:
    RatingStore() = default;
    ~RatingStore();
    RatingStore(const RatingStore&) = delete;
    RatingStore& operator=(const RatingStore&) = delete;

    void Initialize(HWND hwnd);
    void Shutdown();

    // Cache-only lookup: no disk access, safe on the UI thread and on any hot
    // path. Returns nothing when this item has not been read yet.
    std::optional<QuickView::Rating::Resolved> TryGet(ImageID id) const;

    // Queue a background read. `rawPath` is the hidden RAW of a folded pair
    // (empty when the item is not paired); a standalone RAW is passed as
    // `renderedPath` and resolves through its own sidecar.
    void QueueRead(ImageID id, const std::wstring& renderedPath,
                   const std::wstring& rawPath = std::wstring());

    // Drop everything and cancel in-flight work (folder changed).
    void Clear();

    // The .xmp sidecar a RAW's rating lives in, i.e. the path with its
    // extension replaced. Empty when `path` has no extension.
    static std::wstring SidecarPathFor(const std::wstring& path);

    // Read one file's rating straight from disk, no cache. Exposed for the
    // writer (which must re-read to confirm) and for tests of the disk path.
    static std::optional<int> ReadRatingFromFile(const std::wstring& path);
    static std::optional<int> ReadRatingFromSidecar(const std::wstring& sidecarPath);

private:
    struct Task {
        ImageID id = 0;
        std::wstring renderedPath;
        std::wstring rawPath;
        uint64_t generation = 0;
    };

    void WorkerLoop();

    HWND m_hwnd = nullptr;

    mutable std::mutex m_cacheMutex;
    std::unordered_map<ImageID, QuickView::Rating::Resolved> m_cache;

    std::mutex m_queueMutex;
    std::condition_variable m_cv;
    std::deque<Task> m_queue;
    std::unordered_set<ImageID> m_pending;

    std::thread m_worker;
    std::atomic<bool> m_running{ false };
    std::atomic<uint64_t> m_generation{ 0 };
};
