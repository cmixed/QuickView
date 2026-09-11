/*
 * QuickView Star Ratings - metadata parsing (pure, no WIC / no Win32)
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

#include "RatingMetadata.h"

#include <brotli/decode.h>
#include <charconv>
#include <cstring>
#include <vector>

namespace QuickView::Rating {

namespace {

// --- byte readers (every one bounds-checked: this parses untrusted files) ---

uint16_t ReadU16(std::span<const uint8_t> b, size_t off, bool bigEndian) {
    const uint16_t hi = b[off], lo = b[off + 1];
    return bigEndian ? (uint16_t)((hi << 8) | lo) : (uint16_t)((lo << 8) | hi);
}

uint32_t ReadU32(std::span<const uint8_t> b, size_t off, bool bigEndian) {
    const uint32_t b0 = b[off], b1 = b[off + 1], b2 = b[off + 2], b3 = b[off + 3];
    return bigEndian ? (b0 << 24) | (b1 << 16) | (b2 << 8) | b3
                     : (b3 << 24) | (b2 << 16) | (b1 << 8) | b0;
}

uint64_t ReadU64(std::span<const uint8_t> b, size_t off, bool bigEndian) {
    const uint64_t hi = ReadU32(b, off, bigEndian);
    const uint64_t lo = ReadU32(b, off + 4, bigEndian);
    return bigEndian ? (hi << 32) | lo : (lo << 32) | hi;
}

uint64_t ReadVarInt(std::span<const uint8_t> b, size_t off, size_t numBytes) {
    if (numBytes == 0 || off + numBytes > b.size()) return 0;
    uint64_t val = 0;
    for (size_t i = 0; i < numBytes; ++i) {
        val = (val << 8) | b[off + i];
    }
    return val;
}

bool StartsWith(std::span<const uint8_t> b, size_t off, const char* literal, size_t len) {
    if (off + len > b.size()) return false;
    return std::memcmp(b.data() + off, literal, len) == 0;
}

// TIFF/Exif IFD0 scan for tag 0x4746 (SimpleRating). `tiff` starts at the TIFF
// header ("II"/"MM"), which is how Exif nests inside APP1.
std::optional<int> ParseExifRating(std::span<const uint8_t> tiff) {
    constexpr uint16_t TAG_SIMPLE_RATING = 0x4746; // 18246

    if (tiff.size() < 8) return std::nullopt;

    bool bigEndian;
    if (tiff[0] == 'I' && tiff[1] == 'I')      bigEndian = false;
    else if (tiff[0] == 'M' && tiff[1] == 'M') bigEndian = true;
    else return std::nullopt;

    if (ReadU16(tiff, 2, bigEndian) != 42) return std::nullopt;

    const uint32_t ifdOffset = ReadU32(tiff, 4, bigEndian);
    if (ifdOffset < 8 || (size_t)ifdOffset + 2 > tiff.size()) return std::nullopt;

    const uint16_t entryCount = ReadU16(tiff, ifdOffset, bigEndian);
    // 12 bytes per entry; reject a count that cannot fit in the buffer.
    if (entryCount == 0 || (size_t)ifdOffset + 2 + (size_t)entryCount * 12 > tiff.size()) {
        return std::nullopt;
    }

    for (uint16_t i = 0; i < entryCount; ++i) {
        const size_t entry = (size_t)ifdOffset + 2 + (size_t)i * 12;
        if (ReadU16(tiff, entry, bigEndian) != TAG_SIMPLE_RATING) continue;

        const uint16_t type = ReadU16(tiff, entry + 2, bigEndian);
        const uint32_t count = ReadU32(tiff, entry + 4, bigEndian);
        if (count != 1) return std::nullopt;

        // SHORT is what the policy specifies; BYTE and LONG are accepted
        // defensively. All three fit inline in the value field.
        int value;
        switch (type) {
            case 1: value = tiff[entry + 8]; break;                          // BYTE
            case 3: value = ReadU16(tiff, entry + 8, bigEndian); break;      // SHORT
            case 4: value = (int)ReadU32(tiff, entry + 8, bigEndian); break; // LONG
            default: return std::nullopt;
        }
        return IsValidRating(value) ? std::optional<int>(value) : std::nullopt;
    }
    return std::nullopt;
}

} // namespace

std::optional<int> ParseXmpRating(std::string_view xmp) {
    constexpr std::string_view PROPERTY = "xmp:Rating";

    for (size_t pos = xmp.find(PROPERTY); pos != std::string_view::npos;
         pos = xmp.find(PROPERTY, pos + PROPERTY.size())) {
        size_t cursor = pos + PROPERTY.size();

        // Attribute form: xmp:Rating="3" (spaces tolerated around the =)
        // Element form:   <xmp:Rating>3</xmp:Rating>
        while (cursor < xmp.size() && (xmp[cursor] == ' ' || xmp[cursor] == '\t')) ++cursor;
        if (cursor >= xmp.size()) break;

        if (xmp[cursor] == '=') {
            ++cursor;
            while (cursor < xmp.size() && (xmp[cursor] == ' ' || xmp[cursor] == '\t')) ++cursor;
            const char quote = (cursor < xmp.size()) ? xmp[cursor] : '\0';
            if (quote != '"' && quote != '\'') continue;
            ++cursor;
        } else if (xmp[cursor] == '>') {
            ++cursor;
        } else {
            continue; // a longer property name that merely starts with xmp:Rating
        }

        while (cursor < xmp.size() && (xmp[cursor] == ' ' || xmp[cursor] == '\t' ||
                                       xmp[cursor] == '\r' || xmp[cursor] == '\n')) {
            ++cursor;
        }

        size_t end = cursor;
        if (end < xmp.size() && xmp[end] == '-') ++end;             // rejected: -1
        while (end < xmp.size() && xmp[end] >= '0' && xmp[end] <= '9') ++end;
        if (end == cursor) continue;                                 // no digits

        int value = 0;
        const auto result = std::from_chars(xmp.data() + cursor, xmp.data() + end, value);
        if (result.ec != std::errc{} || result.ptr != xmp.data() + end) continue;
        // A fractional rating (xmp:Rating="3.5") is legal XMP; the integer part
        // is what a 0-5 star UI can represent, so it is taken as-is.
        if (IsValidRating(value)) return value;
    }
    return std::nullopt;
}

std::optional<int> ParseTiffRating(std::span<const uint8_t> bytes) {
    return ParseExifRating(bytes);
}

std::optional<int> ParseJpegRating(std::span<const uint8_t> bytes) {
    constexpr size_t EXIF_ID_LEN = 6;  // "Exif\0\0"
    constexpr size_t XMP_ID_LEN = 29;  // "http://ns.adobe.com/xap/1.0/\0"

    if (bytes.size() < 4 || bytes[0] != 0xFF || bytes[1] != 0xD8) {
        return std::nullopt; // not a JPEG (no SOI)
    }

    std::optional<int> fromXmp;
    size_t pos = 2;

    while (pos + 4 <= bytes.size()) {
        if (bytes[pos] != 0xFF) return fromXmp; // desynchronized: stop, stay safe
        const uint8_t marker = bytes[pos + 1];

        // Fill bytes between segments are legal.
        if (marker == 0xFF) { ++pos; continue; }
        // SOS starts the entropy-coded scan and EOI ends the file; no metadata
        // follows either, so there is nothing left worth reading.
        if (marker == 0xDA || marker == 0xD9) break;
        // Standalone markers carry no payload.
        if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) { pos += 2; continue; }

        const uint16_t segLength = ReadU16(bytes, pos + 2, /*bigEndian*/ true);
        if (segLength < 2) return fromXmp;                        // malformed
        const size_t payload = pos + 4;
        const size_t payloadLen = (size_t)segLength - 2;
        if (payload + payloadLen > bytes.size()) {
            // Truncated by our header-sized read: what we already have is the
            // best answer available without reading more of the file.
            return fromXmp;
        }

        if (marker == 0xE1) { // APP1: Exif or XMP
            if (payloadLen >= EXIF_ID_LEN && StartsWith(bytes, payload, "Exif\0\0", EXIF_ID_LEN)) {
                if (auto exifRating = ParseExifRating(
                        bytes.subspan(payload + EXIF_ID_LEN, payloadLen - EXIF_ID_LEN))) {
                    return exifRating; // cheapest and most authoritative source
                }
            } else if (payloadLen >= XMP_ID_LEN && StartsWith(bytes, payload, "http://ns.adobe.com/xap/1.0/\0", XMP_ID_LEN)) {
                if (!fromXmp) {
                    const char* text =
                        reinterpret_cast<const char*>(bytes.data()) + payload + XMP_ID_LEN;
                    fromXmp = ParseXmpRating(std::string_view(text, payloadLen - XMP_ID_LEN));
                }
            }
        }
        pos = payload + payloadLen;
    }
    return fromXmp;
}

std::optional<int> ParsePngRating(std::span<const uint8_t> bytes) {
    // PNG signature: 89 50 4E 47 0D 0A 1A 0A
    static constexpr uint8_t PNG_SIG[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
    if (bytes.size() < 8 || std::memcmp(bytes.data(), PNG_SIG, 8) != 0) {
        return std::nullopt;
    }

    std::optional<int> fromXmp;
    size_t pos = 8; // skip 8-byte signature

    // Each chunk consists of: 4-byte length, 4-byte type, length bytes data, 4-byte CRC
    while (pos + 8 <= bytes.size()) {
        const uint32_t chunkLength = ReadU32(bytes, pos, /*bigEndian*/ true);
        const size_t typePos = pos + 4;
        const size_t dataPos = pos + 8;

        if ((size_t)chunkLength > bytes.size() || dataPos + (size_t)chunkLength + 4 > bytes.size()) {
            return fromXmp; // buffer truncated: return what has been discovered
        }

        const uint8_t* type = bytes.data() + typePos;

        // Stop scanning at IDAT (first pixel stream chunk) or IEND
        if (type[0] == 'I' && type[1] == 'D' && type[2] == 'A' && type[3] == 'T') {
            break;
        }
        if (type[0] == 'I' && type[1] == 'E' && type[2] == 'N' && type[3] == 'D') {
            break;
        }

        // 1. eXIf chunk: raw Exif/TIFF payload
        if (type[0] == 'e' && type[1] == 'X' && type[2] == 'I' && type[3] == 'f') {
            if (chunkLength >= 8) {
                if (auto exifRating = ParseExifRating(bytes.subspan(dataPos, chunkLength))) {
                    return exifRating;
                }
            }
        }
        // 2. iTXt chunk: uncompressed XML:com.adobe.xmp
        else if (type[0] == 'i' && type[1] == 'T' && type[2] == 'X' && type[3] == 't') {
            static constexpr std::string_view XMP_KEYWORD = "XML:com.adobe.xmp";
            if (chunkLength > XMP_KEYWORD.size() + 5) {
                const auto chunkData = bytes.subspan(dataPos, chunkLength);
                if (std::memcmp(chunkData.data(), XMP_KEYWORD.data(), XMP_KEYWORD.size()) == 0 &&
                    chunkData[XMP_KEYWORD.size()] == '\0') {
                    size_t cur = XMP_KEYWORD.size() + 1;
                    if (cur + 2 <= chunkData.size()) {
                        const uint8_t compFlag = chunkData[cur];
                        cur += 2; // skip compression flag and method
                        if (compFlag == 0) { // uncompressed
                            // skip null-terminated language tag
                            while (cur < chunkData.size() && chunkData[cur] != '\0') ++cur;
                            if (cur < chunkData.size()) ++cur;

                            // skip null-terminated translated keyword
                            while (cur < chunkData.size() && chunkData[cur] != '\0') ++cur;
                            if (cur < chunkData.size()) ++cur;

                            if (cur < chunkData.size() && !fromXmp) {
                                const char* text = reinterpret_cast<const char*>(chunkData.data() + cur);
                                const size_t textLen = chunkData.size() - cur;
                                fromXmp = ParseXmpRating(std::string_view(text, textLen));
                            }
                        }
                    }
                }
            }
        }

        pos = dataPos + chunkLength + 4; // advance past chunk data and 4-byte CRC
    }

    return fromXmp;
}

std::optional<int> ParseWebpRating(std::span<const uint8_t> bytes) {
    // RIFF WebP header: 'R''I''F''F' <size> 'W''E''B''P'
    if (bytes.size() < 12) return std::nullopt;
    if (std::memcmp(bytes.data(), "RIFF", 4) != 0 ||
        std::memcmp(bytes.data() + 8, "WEBP", 4) != 0) {
        return std::nullopt;
    }

    std::optional<int> fromXmp;
    size_t pos = 12; // first chunk begins after "WEBP"

    while (pos + 8 <= bytes.size()) {
        const uint8_t* fourcc = bytes.data() + pos;
        const uint32_t chunkLength = ReadU32(bytes, pos + 4, /*bigEndian*/ false);
        const size_t dataPos = pos + 8;

        if ((size_t)chunkLength > bytes.size() || dataPos + (size_t)chunkLength > bytes.size()) {
            return fromXmp; // buffer truncated by header read
        }

        // 1. EXIF chunk
        if (fourcc[0] == 'E' && fourcc[1] == 'X' && fourcc[2] == 'I' && fourcc[3] == 'F') {
            size_t exifOffset = 0;
            // Handle optional "Exif\0\0" header if present
            if (chunkLength >= 6 && StartsWith(bytes, dataPos, "Exif\0\0", 6)) {
                exifOffset = 6;
            }
            if (chunkLength > exifOffset + 8) {
                if (auto exifRating = ParseExifRating(bytes.subspan(dataPos + exifOffset, chunkLength - exifOffset))) {
                    return exifRating;
                }
            }
        }
        // 2. XMP  chunk (FourCC has a trailing space)
        else if (fourcc[0] == 'X' && fourcc[1] == 'M' && fourcc[2] == 'P' && fourcc[3] == ' ') {
            if (!fromXmp && chunkLength > 0) {
                const char* text = reinterpret_cast<const char*>(bytes.data() + dataPos);
                fromXmp = ParseXmpRating(std::string_view(text, chunkLength));
            }
        }

        // Advance to next chunk; odd-sized chunks have a 1-byte padding byte
        pos = dataPos + chunkLength + (chunkLength & 1);
    }

    return fromXmp;
}

std::optional<int> ParseIsobmffRating(std::span<const uint8_t> bytes) {
    if (bytes.size() < 12) return std::nullopt;
    if (std::memcmp(bytes.data() + 4, "ftyp", 4) != 0) return std::nullopt;

    // Scan top-level boxes to locate 'meta'
    size_t pos = 0;
    size_t metaPayloadPos = 0;
    size_t metaEnd = 0;

    while (pos + 8 <= bytes.size()) {
        const uint32_t rawSize = ReadU32(bytes, pos, /*bigEndian*/ true);
        const uint8_t* type = bytes.data() + pos + 4;
        size_t boxSize = rawSize;
        size_t headerSize = 8;

        if (rawSize == 1) { // 64-bit extended size
            if (pos + 16 > bytes.size()) break;
            const uint64_t extSize = ReadU64(bytes, pos + 8, /*bigEndian*/ true);
            if (extSize < 16) break;
            boxSize = static_cast<size_t>(extSize);
            headerSize = 16;
        } else if (rawSize == 0) { // spans to end of file
            boxSize = bytes.size() - pos;
        }

        if (boxSize < headerSize) break;
        const size_t nextPos = pos + boxSize;

        if (std::memcmp(type, "meta", 4) == 0) {
            // 'meta' in ISOBMFF is a FullBox: 4 bytes version + flags after box header
            if (pos + headerSize + 4 <= bytes.size()) {
                metaPayloadPos = pos + headerSize + 4;
                metaEnd = (std::min)(nextPos, bytes.size());
                break;
            }
        }

        if (nextPos <= pos || nextPos > bytes.size()) break;
        pos = nextPos;
    }

    if (metaPayloadPos == 0 || metaPayloadPos >= metaEnd) return std::nullopt;

    // Locate 'iinf' and 'iloc' inside the 'meta' box
    std::span<const uint8_t> iinfSpan;
    std::span<const uint8_t> ilocSpan;
    std::span<const uint8_t> pitmSpan;
    std::span<const uint8_t> irefSpan;

    size_t subPos = metaPayloadPos;
    while (subPos + 8 <= metaEnd) {
        const uint32_t subSize = ReadU32(bytes, subPos, /*bigEndian*/ true);
        if (subSize < 8 || subPos + subSize > metaEnd) break;
        const uint8_t* subType = bytes.data() + subPos + 4;

        if (std::memcmp(subType, "iinf", 4) == 0) {
            iinfSpan = bytes.subspan(subPos, subSize);
        } else if (std::memcmp(subType, "iloc", 4) == 0) {
            ilocSpan = bytes.subspan(subPos, subSize);
        } else if (std::memcmp(subType, "pitm", 4) == 0) {
            pitmSpan = bytes.subspan(subPos, subSize);
        } else if (std::memcmp(subType, "iref", 4) == 0) {
            irefSpan = bytes.subspan(subPos, subSize);
        }

        subPos += subSize;
    }

    if (iinfSpan.size() < 12 || ilocSpan.size() < 16) return std::nullopt;

    uint32_t primaryItemId = 1;
    if (pitmSpan.size() >= 14) {
        const uint8_t pitmVer = pitmSpan[8];
        if (pitmVer == 0 && pitmSpan.size() >= 14) {
            primaryItemId = ReadU16(pitmSpan, 12, true);
        } else if (pitmVer == 1 && pitmSpan.size() >= 16) {
            primaryItemId = ReadU32(pitmSpan, 12, true);
        }
    }

    std::vector<uint32_t> primaryCdscItems;
    if (irefSpan.size() >= 12) {
        const uint8_t irefVer = irefSpan[8];
        size_t irefCur = 12;
        while (irefCur + 8 <= irefSpan.size()) {
            const uint32_t boxSz = ReadU32(irefSpan, irefCur, true);
            if (boxSz < 8 || irefCur + boxSz > irefSpan.size()) break;
            const uint8_t* btype = irefSpan.data() + irefCur + 4;
            if (std::memcmp(btype, "cdsc", 4) == 0) {
                if (irefVer == 0 && boxSz >= 14) {
                    const uint16_t fromId = ReadU16(irefSpan, irefCur + 8, true);
                    const uint16_t refCnt = ReadU16(irefSpan, irefCur + 10, true);
                    for (uint16_t r = 0; r < refCnt && irefCur + 12 + (r + 1) * 2 <= irefCur + boxSz; ++r) {
                        const uint16_t toId = ReadU16(irefSpan, irefCur + 12 + r * 2, true);
                        if (toId == primaryItemId) {
                            primaryCdscItems.push_back(fromId);
                            break;
                        }
                    }
                } else if (irefVer == 1 && boxSz >= 18) {
                    const uint32_t fromId = ReadU32(irefSpan, irefCur + 8, true);
                    const uint16_t refCnt = ReadU16(irefSpan, irefCur + 12, true);
                    for (uint16_t r = 0; r < refCnt && irefCur + 14 + (r + 1) * 4 <= irefCur + boxSz; ++r) {
                        const uint32_t toId = ReadU32(irefSpan, irefCur + 14 + r * 4, true);
                        if (toId == primaryItemId) {
                            primaryCdscItems.push_back(fromId);
                            break;
                        }
                    }
                }
            }
            irefCur += boxSz;
        }
    }

    auto isCdscForPrimary = [&](uint32_t id) {
        return std::find(primaryCdscItems.begin(), primaryCdscItems.end(), id) != primaryCdscItems.end();
    };

    // Parse 'iinf' to find item IDs for 'Exif' and 'mime' (XMP)
    uint32_t exifItemId = 0;
    uint32_t xmpItemId = 0;
    bool exifLocked = false;
    bool xmpLocked = false;

    const uint8_t iinfVer = iinfSpan[8]; // FullBox version
    size_t iinfCur = 12;
    uint32_t entryCount = 0;
    if (iinfVer == 0) {
        if (iinfCur + 2 > iinfSpan.size()) return std::nullopt;
        entryCount = ReadU16(iinfSpan, iinfCur, true);
        iinfCur += 2;
    } else {
        if (iinfCur + 4 > iinfSpan.size()) return std::nullopt;
        entryCount = ReadU32(iinfSpan, iinfCur, true);
        iinfCur += 4;
    }

    for (uint32_t i = 0; i < entryCount && iinfCur + 8 <= iinfSpan.size(); ++i) {
        const uint32_t infeSize = ReadU32(iinfSpan, iinfCur, true);
        if (infeSize < 8 || iinfCur + infeSize > iinfSpan.size()) break;

        if (std::memcmp(iinfSpan.data() + iinfCur + 4, "infe", 4) == 0 && infeSize >= 12) {
            const uint8_t infeVer = iinfSpan[iinfCur + 8];
            if (infeVer >= 2) {
                uint32_t itemId = 0;
                size_t typeOffset = 0;
                if (infeVer == 2) {
                    if (infeSize >= 16) {
                        itemId = ReadU16(iinfSpan, iinfCur + 12, true);
                        typeOffset = iinfCur + 16;
                    }
                } else if (infeVer == 3) {
                    if (infeSize >= 18) {
                        itemId = ReadU32(iinfSpan, iinfCur + 12, true);
                        typeOffset = iinfCur + 18;
                    }
                }

                if (typeOffset > 0 && typeOffset + 4 <= iinfCur + infeSize) {
                    const uint8_t* itype = iinfSpan.data() + typeOffset;
                    if (std::memcmp(itype, "Exif", 4) == 0) {
                        if (isCdscForPrimary(itemId)) {
                            if (!exifLocked) {
                                exifItemId = itemId;
                                exifLocked = true;
                            }
                        } else if (!exifLocked && exifItemId == 0) {
                            exifItemId = itemId;
                        }
                    } else if (std::memcmp(itype, "mime", 4) == 0) {
                        std::string_view rest(
                            reinterpret_cast<const char*>(iinfSpan.data() + typeOffset + 4),
                            infeSize - (typeOffset + 4 - iinfCur));
                        if (rest.find("application/rdf+xml") != std::string_view::npos ||
                            rest.find("application/x-xmp") != std::string_view::npos ||
                            rest.find("xmp") != std::string_view::npos) {
                            if (isCdscForPrimary(itemId)) {
                                if (!xmpLocked) {
                                    xmpItemId = itemId;
                                    xmpLocked = true;
                                }
                            } else if (!xmpLocked && xmpItemId == 0) {
                                xmpItemId = itemId;
                            }
                        }
                    }
                }
            }
        }
        iinfCur += infeSize;
    }

    if (exifItemId == 0 && xmpItemId == 0) return std::nullopt;

    // Parse 'iloc' to find absolute byte spans of the identified items
    const uint8_t ilocVer = ilocSpan[8];
    const uint8_t offSize = (ilocSpan[12] >> 4) & 0x0F;
    const uint8_t lenSize = ilocSpan[12] & 0x0F;
    const uint8_t baseOffSize = (ilocSpan[13] >> 4) & 0x0F;
    const uint8_t indexSize = (ilocVer == 1 || ilocVer == 2) ? (ilocSpan[13] & 0x0F) : 0;

    size_t ilocCur = 14;
    uint32_t itemCount = 0;
    if (ilocVer < 2) {
        if (ilocCur + 2 > ilocSpan.size()) return std::nullopt;
        itemCount = ReadU16(ilocSpan, ilocCur, true);
        ilocCur += 2;
    } else {
        if (ilocCur + 4 > ilocSpan.size()) return std::nullopt;
        itemCount = ReadU32(ilocSpan, ilocCur, true);
        ilocCur += 4;
    }

    size_t exifPayloadOff = 0, exifPayloadLen = 0;
    size_t xmpPayloadOff = 0, xmpPayloadLen = 0;

    for (uint32_t i = 0; i < itemCount && ilocCur < ilocSpan.size(); ++i) {
        uint32_t itemId = 0;
        if (ilocVer < 2) {
            if (ilocCur + 2 > ilocSpan.size()) break;
            itemId = ReadU16(ilocSpan, ilocCur, true);
            ilocCur += 2;
        } else {
            if (ilocCur + 4 > ilocSpan.size()) break;
            itemId = ReadU32(ilocSpan, ilocCur, true);
            ilocCur += 4;
        }

        if (ilocVer == 1 || ilocVer == 2) {
            ilocCur += 2; // skip construction_method
        }
        ilocCur += 2; // skip data_reference_index

        const uint64_t baseOffset = ReadVarInt(ilocSpan, ilocCur, baseOffSize);
        ilocCur += baseOffSize;

        if (ilocCur + 2 > ilocSpan.size()) break;
        const uint16_t extentCount = ReadU16(ilocSpan, ilocCur, true);
        ilocCur += 2;

        for (uint16_t e = 0; e < extentCount; ++e) {
            ilocCur += indexSize;
            const uint64_t extentOffset = ReadVarInt(ilocSpan, ilocCur, offSize);
            ilocCur += offSize;
            const uint64_t extentLength = ReadVarInt(ilocSpan, ilocCur, lenSize);
            ilocCur += lenSize;

            if (e == 0) { // Primary extent
                const uint64_t absOffset = baseOffset + extentOffset;
                if (itemId == exifItemId && exifPayloadLen == 0) {
                    exifPayloadOff = static_cast<size_t>(absOffset);
                    exifPayloadLen = static_cast<size_t>(extentLength);
                } else if (itemId == xmpItemId && xmpPayloadLen == 0) {
                    xmpPayloadOff = static_cast<size_t>(absOffset);
                    xmpPayloadLen = static_cast<size_t>(extentLength);
                }
            }
        }
    }

    // 1. Try Exif
    if (exifPayloadLen > 4 && exifPayloadOff + exifPayloadLen <= bytes.size()) {
        const uint32_t tiffOffset = ReadU32(bytes, exifPayloadOff, true);
        size_t actualTiffPos = exifPayloadOff + 4 + tiffOffset;
        if (actualTiffPos + 8 > exifPayloadOff + exifPayloadLen || actualTiffPos >= bytes.size()) {
            actualTiffPos = exifPayloadOff + 4; // fallback to offset 4
        }
        if (actualTiffPos + 8 <= bytes.size()) {
            const size_t avail = (std::min)(bytes.size() - actualTiffPos, exifPayloadLen - (actualTiffPos - exifPayloadOff));
            if (auto rating = ParseExifRating(bytes.subspan(actualTiffPos, avail))) {
                return rating;
            }
        }
    }

    // 2. Try XMP
    if (xmpPayloadLen > 0 && xmpPayloadOff + xmpPayloadLen <= bytes.size()) {
        const char* text = reinterpret_cast<const char*>(bytes.data() + xmpPayloadOff);
        if (auto rating = ParseXmpRating(std::string_view(text, xmpPayloadLen))) {
            return rating;
        }
    }

    return std::nullopt;
}

std::optional<int> ParseJxlRating(std::span<const uint8_t> bytes) {
    if (bytes.size() < 2) return std::nullopt;

    // 1. Bare codestream check: starts with 0xFF 0x0A
    if (bytes[0] == 0xFF && bytes[1] == 0x0A) {
        return std::nullopt; // Bare codestreams carry no metadata
    }

    // 2. Container signature check: 00 00 00 0C 4A 58 4C 20 0D 0A 87 0A
    static constexpr uint8_t JXL_SIG[12] = {
        0x00, 0x00, 0x00, 0x0C, 'J', 'X', 'L', ' ', 0x0D, 0x0A, 0x87, 0x0A
    };
    if (bytes.size() < 12 || std::memcmp(bytes.data(), JXL_SIG, 12) != 0) {
        return std::nullopt;
    }

    std::optional<int> fromXmp;
    size_t pos = 12; // Start after JXL signature box

    while (pos + 8 <= bytes.size()) {
        uint64_t boxSize = ReadU32(bytes, pos, /*bigEndian=*/true);
        const uint8_t* type = bytes.data() + pos + 4;
        size_t hdrSize = 8;

        if (boxSize == 1) {
            // Extended 64-bit size
            if (pos + 16 > bytes.size()) break;
            boxSize = ReadU64(bytes, pos + 8, /*bigEndian=*/true);
            hdrSize = 16;
        } else if (boxSize == 0) {
            // Box extends to EOF
            boxSize = bytes.size() - pos;
        }

        if (boxSize < hdrSize || pos + boxSize > bytes.size()) {
            break; // Corrupted or truncated box
        }

        const size_t payloadOff = pos + hdrSize;
        const size_t payloadLen = static_cast<size_t>(boxSize - hdrSize);

        // 1. Uncompressed Exif box: [4-byte offset][TIFF data]
        if (std::memcmp(type, "Exif", 4) == 0) {
            if (payloadLen > 4) {
                const uint32_t tiffOffset = ReadU32(bytes, payloadOff, /*bigEndian=*/true);
                size_t actualTiffPos = payloadOff + 4 + tiffOffset;
                if (actualTiffPos + 8 > payloadOff + payloadLen || actualTiffPos >= bytes.size()) {
                    actualTiffPos = payloadOff + 4; // fallback to offset 4
                }
                if (actualTiffPos + 8 <= bytes.size()) {
                    const size_t avail = (std::min)(bytes.size() - actualTiffPos, payloadLen - (actualTiffPos - payloadOff));
                    if (auto r = ParseTiffRating(std::span<const uint8_t>(bytes.data() + actualTiffPos, avail))) {
                        return r;
                    }
                }
            }
        }
        // 2. Uncompressed xml box: raw UTF-8 XMP string
        else if (std::memcmp(type, "xml ", 4) == 0) {
            if (!fromXmp && payloadLen > 0) {
                std::string_view xmpStr(reinterpret_cast<const char*>(bytes.data() + payloadOff), payloadLen);
                fromXmp = ParseXmpRating(xmpStr);
            }
        }
        // 3. Brotli-compressed box: [4-byte original_box_type][brotli stream]
        else if (std::memcmp(type, "brob", 4) == 0) {
            if (payloadLen > 4) {
                const uint8_t* origType = bytes.data() + payloadOff;
                const uint8_t* compData = bytes.data() + payloadOff + 4;
                const size_t compLen = payloadLen - 4;

                if (std::memcmp(origType, "Exif", 4) == 0) {
                    std::vector<uint8_t> decomp(65536);
                    size_t decompLen = decomp.size();
                    if (BrotliDecoderDecompress(compLen, compData, &decompLen, decomp.data()) == BROTLI_DECODER_RESULT_SUCCESS) {
                        if (decompLen > 4) {
                            const uint32_t tiffOffset = (static_cast<uint32_t>(decomp[0]) << 24) |
                                                        (static_cast<uint32_t>(decomp[1]) << 16) |
                                                        (static_cast<uint32_t>(decomp[2]) << 8)  |
                                                         static_cast<uint32_t>(decomp[3]);
                            size_t actualTiffPos = 4 + tiffOffset;
                            if (actualTiffPos + 8 > decompLen) actualTiffPos = 4;
                            if (actualTiffPos + 8 <= decompLen) {
                                if (auto r = ParseTiffRating(std::span<const uint8_t>(decomp.data() + actualTiffPos, decompLen - actualTiffPos))) {
                                    return r;
                                }
                            }
                        }
                    }
                } else if (std::memcmp(origType, "xml ", 4) == 0 && !fromXmp) {
                    std::vector<uint8_t> decomp(131072);
                    size_t decompLen = decomp.size();
                    if (BrotliDecoderDecompress(compLen, compData, &decompLen, decomp.data()) == BROTLI_DECODER_RESULT_SUCCESS) {
                        std::string_view xmpStr(reinterpret_cast<const char*>(decomp.data()), decompLen);
                        fromXmp = ParseXmpRating(xmpStr);
                    }
                }
            }
        }

        pos += static_cast<size_t>(boxSize);
    }

    return fromXmp;
}

namespace {

// Locate an existing xmp:Rating and report the exact span to replace, which is
// what makes the update surgical: everything outside [start, end) is copied
// through untouched.
struct PropertySpan {
    size_t start = 0;   // first character of the whole property
    size_t end = 0;     // one past its last character
    bool attributeForm = false;
};

std::optional<PropertySpan> FindRatingProperty(std::string_view xmp) {
    constexpr std::string_view NAME = "xmp:Rating";

    for (size_t pos = xmp.find(NAME); pos != std::string_view::npos;
         pos = xmp.find(NAME, pos + NAME.size())) {
        size_t cursor = pos + NAME.size();
        while (cursor < xmp.size() && (xmp[cursor] == ' ' || xmp[cursor] == '\t')) ++cursor;
        if (cursor >= xmp.size()) break;

        if (xmp[cursor] == '=') {
            // xmp:Rating="3"
            ++cursor;
            while (cursor < xmp.size() && (xmp[cursor] == ' ' || xmp[cursor] == '\t')) ++cursor;
            if (cursor >= xmp.size()) break;
            const char quote = xmp[cursor];
            if (quote != '"' && quote != '\'') continue;
            const size_t close = xmp.find(quote, cursor + 1);
            if (close == std::string_view::npos) continue;
            return PropertySpan{ pos, close + 1, true };
        }
        if (xmp[cursor] == '>') {
            // <xmp:Rating>3</xmp:Rating>: the span starts at the opening '<'
            constexpr std::string_view CLOSE_TAG = "</xmp:Rating>";
            const size_t close = xmp.find(CLOSE_TAG, cursor);
            if (close == std::string_view::npos) continue;
            const size_t openTag = xmp.rfind('<', pos);
            if (openTag == std::string_view::npos) continue;
            return PropertySpan{ openTag, close + CLOSE_TAG.size(), false };
        }
    }
    return std::nullopt;
}

} // namespace

std::optional<std::string> UpdateXmpRating(std::string_view xmp, int stars) {
    if (xmp.empty()) return std::nullopt;

    const std::string value = std::to_string(stars);

    if (const auto span = FindRatingProperty(xmp)) {
        std::string out;
        out.reserve(xmp.size() + 16);
        out.append(xmp.substr(0, span->start));
        if (stars > MIN_STARS) {
            out.append(span->attributeForm ? "xmp:Rating=\"" + value + "\""
                                           : "<xmp:Rating>" + value + "</xmp:Rating>");
        }
        // stars == 0 drops the property entirely, which is what clearing means.
        out.append(xmp.substr(span->end));
        return out;
    }

    if (stars <= MIN_STARS) {
        return std::string(xmp); // nothing to clear, leave the document as it is
    }

    // Absent: add it as an attribute of the first rdf:Description, which is
    // where Lightroom and Capture One keep it too.
    constexpr std::string_view DESCRIPTION = "<rdf:Description";
    const size_t desc = xmp.find(DESCRIPTION);
    if (desc == std::string_view::npos) return std::nullopt; // unfamiliar shape

    const size_t insert = desc + DESCRIPTION.size();
    std::string out;
    out.reserve(xmp.size() + 32);
    out.append(xmp.substr(0, insert));
    out.append("\n    xmp:Rating=\"" + value + "\"");
    out.append(xmp.substr(insert));
    return out;
}

std::string BuildMinimalXmp(int stars) {
    const std::string value = std::to_string(stars > MIN_STARS ? stars : MIN_STARS);
    return
        "<?xpacket begin=\"\xEF\xBB\xBF\" id=\"W5M0MpCehiHzreSzNTczkc9d\"?>\n"
        "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\">\n"
        " <rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">\n"
        "  <rdf:Description rdf:about=\"\"\n"
        "    xmlns:xmp=\"http://ns.adobe.com/xap/1.0/\"\n"
        "    xmp:Rating=\"" + value + "\"/>\n"
        " </rdf:RDF>\n"
        "</x:xmpmeta>\n"
        "<?xpacket end=\"w\"?>\n";
}

std::optional<std::string> PatchXmpRatingInPlaceStrict(std::string_view xmp, int stars) {
    if (xmp.empty()) return std::nullopt;

    constexpr std::string_view PACKET_END = "<?xpacket end=";
    const size_t endPos = xmp.rfind(PACKET_END);
    size_t wsEnd = (endPos != std::string_view::npos) ? endPos : xmp.size();
    size_t wsStart = wsEnd;
    while (wsStart > 0 && (xmp[wsStart - 1] == ' ' || xmp[wsStart - 1] == '\t' ||
                           xmp[wsStart - 1] == '\r' || xmp[wsStart - 1] == '\n')) {
        --wsStart;
    }
    const size_t availWs = wsEnd - wsStart;

    const auto span = FindRatingProperty(xmp);
    if (span && span->end <= wsStart) {
        std::string out;
        out.reserve(xmp.size());
        const size_t oldLen = span->end - span->start;

        if (stars > MIN_STARS) {
            const std::string value = std::to_string(stars);
            const std::string replacement = span->attributeForm ? "xmp:Rating=\"" + value + "\""
                                                                : "<xmp:Rating>" + value + "</xmp:Rating>";
            const int64_t delta = static_cast<int64_t>(replacement.size()) - static_cast<int64_t>(oldLen);
            if (delta > 0 && static_cast<size_t>(delta) > availWs) {
                return std::nullopt;
            }
            out.append(xmp.substr(0, span->start));
            out.append(replacement);
            out.append(xmp.substr(span->end, wsStart - span->end));
            const size_t newWsLen = static_cast<size_t>(static_cast<int64_t>(availWs) - delta);
            out.append(newWsLen, ' ');
            out.append(xmp.substr(wsEnd));
        } else {
            // Clearing: drop the property, reclaim oldLen into whitespace padding
            out.append(xmp.substr(0, span->start));
            out.append(xmp.substr(span->end, wsStart - span->end));
            out.append(availWs + oldLen, ' ');
            out.append(xmp.substr(wsEnd));
        }

        if (out.size() == xmp.size()) return out;
        return std::nullopt;
    }

    if (stars <= MIN_STARS) {
        return std::string(xmp);
    }

    constexpr std::string_view DESCRIPTION = "<rdf:Description";
    const size_t desc = xmp.find(DESCRIPTION);
    if (desc == std::string_view::npos) return std::nullopt;

    const size_t insertPos = desc + DESCRIPTION.size();
    if (insertPos > wsStart) return std::nullopt;

    const std::string snippet = "\n    xmp:Rating=\"" + std::to_string(stars) + "\"";
    const size_t delta = snippet.size();
    if (delta > availWs) return std::nullopt;

    std::string out;
    out.reserve(xmp.size());
    out.append(xmp.substr(0, insertPos));
    out.append(snippet);
    out.append(xmp.substr(insertPos, wsStart - insertPos));
    out.append(availWs - delta, ' ');
    out.append(xmp.substr(wsEnd));

    if (out.size() == xmp.size()) return out;
    return std::nullopt;
}

Resolved ResolvePairRating(std::optional<int> inFile, std::optional<int> sidecar) {
    // Rejected (-1) is a deliberate mark, so it takes part in the resolution;
    // it is only flattened to 0 stars for display.
    auto toStars = [](int value) { return value < MIN_STARS ? MIN_STARS : value; };

    Resolved out;
    if (sidecar && inFile) {
        out.stars = toStars(*sidecar);
        out.source = Source::Sidecar;
        out.conflict = (*sidecar != *inFile);
        out.otherStars = toStars(*inFile);
    } else if (sidecar) {
        out.stars = toStars(*sidecar);
        out.source = Source::Sidecar;
    } else if (inFile) {
        out.stars = toStars(*inFile);
        out.source = Source::InFile;
    }
    return out;
}

} // namespace QuickView::Rating
