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

#include <charconv>
#include <cstring>

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
            if (StartsWith(bytes, payload, "Exif\0\0", EXIF_ID_LEN)) {
                if (auto exifRating = ParseExifRating(
                        bytes.subspan(payload + EXIF_ID_LEN, payloadLen - EXIF_ID_LEN))) {
                    return exifRating; // cheapest and most authoritative source
                }
            } else if (StartsWith(bytes, payload, "http://ns.adobe.com/xap/1.0/\0", XMP_ID_LEN)) {
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
