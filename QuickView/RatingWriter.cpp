/*
 * QuickView Star Ratings - writing a rating into an image file
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

#include "RatingWriter.h"

#include "pch.h"

#include <wincodecsdk.h>
#include "SupportedExtensions.h"

#include <wrl/client.h>

#include <vector>

using Microsoft::WRL::ComPtr;

namespace QuickView::Rating {

namespace {

// A JPEG nests Exif under APP1; a TIFF carries the IFD directly. Both take
// xmp:Rating, which is the property Lightroom and Bridge read.
struct RatingQueries {
    const wchar_t* exif;
    const wchar_t* xmp;
};

RatingQueries QueriesFor(const std::wstring& path) {
    const std::wstring_view ext = QuickView::ExtensionOf(path);
    const bool isTiff = QuickView::ExtEqualsIgnoreCase(ext, L".tif") ||
                        QuickView::ExtEqualsIgnoreCase(ext, L".tiff");
    return isTiff ? RatingQueries{ L"/ifd/{ushort=18246}", L"/ifd/xmp/xmp:Rating" }
                  : RatingQueries{ L"/app1/ifd/{ushort=18246}", L"/xmp/xmp:Rating" };
}

// Writing SimpleRating (0..5) is enough for both ecosystems: Explorer and
// Photos derive System.Rating (0..99) from it, and Adobe reads xmp:Rating.
HRESULT ApplyRating(IWICMetadataQueryWriter* writer, const RatingQueries& q, int stars) {
    if (stars <= 0) {
        // Clearing means removing the property, not storing a zero, so the
        // file goes back to carrying no rating at all. A property that was
        // not there is not an error.
        writer->RemoveMetadataByName(q.exif);
        writer->RemoveMetadataByName(q.xmp);
        return S_OK;
    }

    PROPVARIANT var;
    PropVariantInit(&var);
    var.vt = VT_UI2;
    var.uiVal = (USHORT)stars;
    HRESULT hr = writer->SetMetadataByName(q.exif, &var);
    PropVariantClear(&var);
    if (FAILED(hr)) return hr;

    const std::wstring text = std::to_wstring(stars);
    PropVariantInit(&var);
    var.vt = VT_LPWSTR;
    var.pwszVal = (WCHAR*)CoTaskMemAlloc((text.size() + 1) * sizeof(WCHAR));
    if (!var.pwszVal) return E_OUTOFMEMORY;
    wcscpy_s(var.pwszVal, text.size() + 1, text.c_str());
    hr = writer->SetMetadataByName(q.xmp, &var);
    PropVariantClear(&var);
    // An XMP packet cannot always be added in place; the Exif tag alone still
    // satisfies Explorer, so this is not treated as a failure.
    return S_OK;
}

// Patch the value into the padding the file already has: no re-encode, no
// rewrite, every pixel byte untouched.
HRESULT TryInPlace(IWICImagingFactory* factory, const std::wstring& path, int stars) {
    ComPtr<IWICBitmapDecoder> decoder;
    HRESULT hr = factory->CreateDecoderFromFilename(path.c_str(), nullptr,
                                                    GENERIC_READ | GENERIC_WRITE,
                                                    WICDecodeMetadataCacheOnDemand, &decoder);
    if (FAILED(hr)) return hr;

    ComPtr<IWICFastMetadataEncoder> encoder;
    hr = factory->CreateFastMetadataEncoderFromDecoder(decoder.Get(), &encoder);
    if (FAILED(hr)) return hr;

    ComPtr<IWICMetadataQueryWriter> writer;
    hr = encoder->GetMetadataQueryWriter(&writer);
    if (FAILED(hr)) return hr;

    hr = ApplyRating(writer.Get(), QueriesFor(path), stars);
    if (FAILED(hr)) return hr;

    return encoder->Commit();
}

// Rebuild the file when there was no room to patch. WriteSource with a null
// rectangle copies the compressed frame as it is, so the pixels come through
// bit for bit; only the metadata block is new.
HRESULT Transcode(IWICImagingFactory* factory, const std::wstring& path, int stars) {
    // Decode from a copy held in memory rather than from the file. A decoder
    // opened on the path keeps it open even after every interface has been
    // released, and the swap at the end then fails with a sharing violation.
    std::vector<BYTE> sourceBytes;
    {
        HANDLE file = INVALID_HANDLE_VALUE;
        for (int retry = 0; retry < 5; ++retry) {
            file = CreateFileW(path.c_str(), GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
            if (file != INVALID_HANDLE_VALUE) break;
            Sleep(15);
        }
        if (file == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32(GetLastError());
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 || size.QuadPart > MAXDWORD) {
            CloseHandle(file);
            return E_FAIL;
        }
        sourceBytes.resize((size_t)size.QuadPart);
        DWORD read = 0;
        const BOOL ok = ReadFile(file, sourceBytes.data(), (DWORD)sourceBytes.size(), &read, nullptr);
        CloseHandle(file);
        if (!ok || read != sourceBytes.size()) return E_FAIL;
    }

    ComPtr<IWICStream> sourceStream;
    HRESULT hr = factory->CreateStream(&sourceStream);
    if (FAILED(hr)) return hr;
    hr = sourceStream->InitializeFromMemory(sourceBytes.data(), (DWORD)sourceBytes.size());
    if (FAILED(hr)) return hr;

    ComPtr<IWICBitmapDecoder> decoder;
    hr = factory->CreateDecoderFromStream(sourceStream.Get(), nullptr,
                                          WICDecodeMetadataCacheOnDemand, &decoder);
    if (FAILED(hr)) return hr;

    GUID containerFormat{};
    hr = decoder->GetContainerFormat(&containerFormat);
    if (FAILED(hr)) return hr;

    UINT frameCount = 0;
    hr = decoder->GetFrameCount(&frameCount);
    if (FAILED(hr) || frameCount == 0) return FAILED(hr) ? hr : E_FAIL;

    const std::wstring tempPath = path + L".qvrating.tmp";
    ComPtr<IWICStream> stream;
    hr = factory->CreateStream(&stream);
    if (FAILED(hr)) return hr;
    hr = stream->InitializeFromFilename(tempPath.c_str(), GENERIC_WRITE);
    if (FAILED(hr)) return hr;

    auto dropTemp = [&tempPath] { DeleteFileW(tempPath.c_str()); };

    ComPtr<IWICBitmapEncoder> encoder;
    hr = factory->CreateEncoder(containerFormat, nullptr, &encoder);
    if (FAILED(hr)) { dropTemp(); return hr; }
    hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    if (FAILED(hr)) { dropTemp(); return hr; }

    for (UINT i = 0; i < frameCount; ++i) {
        ComPtr<IWICBitmapFrameDecode> frameDecode;
        hr = decoder->GetFrame(i, &frameDecode);
        if (FAILED(hr)) { dropTemp(); return hr; }

        ComPtr<IWICBitmapFrameEncode> frameEncode;
        hr = encoder->CreateNewFrame(&frameEncode, nullptr);
        if (FAILED(hr)) { dropTemp(); return hr; }
        hr = frameEncode->Initialize(nullptr);
        if (FAILED(hr)) { dropTemp(); return hr; }

        // Carry the existing metadata over before adding the rating to it.
        ComPtr<IWICMetadataBlockReader> blockReader;
        if (SUCCEEDED(frameDecode.As(&blockReader))) {
            ComPtr<IWICMetadataBlockWriter> blockWriter;
            if (SUCCEEDED(frameEncode.As(&blockWriter))) {
                blockWriter->InitializeFromBlockReader(blockReader.Get());
            }
        }

        if (i == 0) {
            ComPtr<IWICMetadataQueryWriter> writer;
            if (SUCCEEDED(frameEncode->GetMetadataQueryWriter(&writer))) {
                ApplyRating(writer.Get(), QueriesFor(path), stars);
            }
        }

        hr = frameEncode->WriteSource(frameDecode.Get(), nullptr);
        if (FAILED(hr)) { dropTemp(); return hr; }
        hr = frameEncode->Commit();
        if (FAILED(hr)) { dropTemp(); return hr; }
    }

    hr = encoder->Commit();
    if (FAILED(hr)) { dropTemp(); return hr; }

    // Release everything that keeps the temp file's stream alive before the
    // swap -- the query writer counts, since it holds the frame encoder, which
    // holds the stream, and ReplaceFileW needs the replacement to itself.
    // (The source file needs no such care: it was decoded from memory, so the
    // original was only ever open for the moment it took to read it.)
    stream.Reset();
    encoder.Reset();
    decoder.Reset();
    sourceStream.Reset();

    BOOL replaceOk = FALSE;
    DWORD lastErr = 0;
    for (int retry = 0; retry < 5; ++retry) {
        if (ReplaceFileW(path.c_str(), tempPath.c_str(), nullptr, REPLACEFILE_IGNORE_MERGE_ERRORS,
                         nullptr, nullptr)) {
            replaceOk = TRUE;
            break;
        }
        lastErr = GetLastError();
        Sleep(20);
    }

    if (!replaceOk) {
        dropTemp();
        return HRESULT_FROM_WIN32(lastErr);
    }
    return S_OK;
}

} // namespace

WriteStatus WriteRatingToImage(const std::wstring& path, int stars, bool allowTranscode) {
    if (path.empty()) return WriteStatus::Failed;

    const std::wstring_view ext = QuickView::ExtensionOf(path);
    const bool isTiff = QuickView::ExtEqualsIgnoreCase(ext, L".tif") ||
                        QuickView::ExtEqualsIgnoreCase(ext, L".tiff");

    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&factory)))) {
        return WriteStatus::Failed;
    }

    // WIC TIFF decoder does NOT support FastMetadataEncoder (returns WINCODEC_ERR_UNSUPPORTEDOPERATION).
    // Bypassing TryInPlace for TIFF avoids open handle collisions during subsequent Transcode.
    if (!isTiff) {
        const HRESULT hrInPlace = TryInPlace(factory.Get(), path, stars);
        if (SUCCEEDED(hrInPlace)) {
            return WriteStatus::WrittenInPlace;
        }
    }

    if (!allowTranscode) return WriteStatus::NeedsTranscode;

    const HRESULT hrTrans = Transcode(factory.Get(), path, stars);
    return SUCCEEDED(hrTrans) ? WriteStatus::WrittenTranscode : WriteStatus::Failed;
}

} // namespace QuickView::Rating
