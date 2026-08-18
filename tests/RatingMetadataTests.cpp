#include <gtest/gtest.h>
#include "RatingMetadata.h"

#include <cstdint>
#include <string>
#include <vector>

using namespace QuickView::Rating;

namespace {

// Test images are assembled in memory on purpose: a unit test that depends on
// a file outside the repository cannot pass on a fresh clone.

void AppendU16BE(std::vector<uint8_t>& out, uint16_t value) {
    out.push_back((uint8_t)(value >> 8));
    out.push_back((uint8_t)(value & 0xFF));
}

void AppendBytes(std::vector<uint8_t>& out, const char* data, size_t len) {
    for (size_t i = 0; i < len; ++i) out.push_back((uint8_t)data[i]);
}

// Minimal TIFF block holding IFD0 with a single tag.
std::vector<uint8_t> MakeTiff(uint16_t tag, uint16_t type, uint32_t value, bool bigEndian) {
    std::vector<uint8_t> t;
    auto u16 = [&](uint16_t v) {
        if (bigEndian) { t.push_back((uint8_t)(v >> 8)); t.push_back((uint8_t)v); }
        else           { t.push_back((uint8_t)v); t.push_back((uint8_t)(v >> 8)); }
    };
    auto u32 = [&](uint32_t v) {
        if (bigEndian) {
            t.push_back((uint8_t)(v >> 24)); t.push_back((uint8_t)(v >> 16));
            t.push_back((uint8_t)(v >> 8));  t.push_back((uint8_t)v);
        } else {
            t.push_back((uint8_t)v);         t.push_back((uint8_t)(v >> 8));
            t.push_back((uint8_t)(v >> 16)); t.push_back((uint8_t)(v >> 24));
        }
    };

    t.push_back(bigEndian ? 'M' : 'I');
    t.push_back(bigEndian ? 'M' : 'I');
    u16(42);
    u32(8);        // IFD0 begins right after the header
    u16(1);        // one entry
    u16(tag);
    u16(type);
    u32(1);        // count
    // TIFF stores a value shorter than 4 bytes left-justified in the value
    // field, so a SHORT occupies the first two bytes (not a padded u32).
    switch (type) {
        case 1: t.push_back((uint8_t)value); t.push_back(0); u16(0); break; // BYTE
        case 3: u16((uint16_t)value); u16(0); break;                        // SHORT
        default: u32(value); break;                                         // LONG
    }
    u32(0);        // no next IFD
    return t;
}

std::vector<uint8_t> MakeJpeg(const std::vector<std::vector<uint8_t>>& app1Payloads) {
    std::vector<uint8_t> jpeg{ 0xFF, 0xD8 }; // SOI
    for (const auto& payload : app1Payloads) {
        jpeg.push_back(0xFF);
        jpeg.push_back(0xE1); // APP1
        AppendU16BE(jpeg, (uint16_t)(payload.size() + 2));
        jpeg.insert(jpeg.end(), payload.begin(), payload.end());
    }
    jpeg.push_back(0xFF);
    jpeg.push_back(0xDA); // SOS - scan data would follow
    return jpeg;
}

std::vector<uint8_t> ExifPayload(uint16_t tag, uint16_t type, uint32_t value, bool bigEndian) {
    std::vector<uint8_t> payload;
    AppendBytes(payload, "Exif\0\0", 6);
    const std::vector<uint8_t> tiff = MakeTiff(tag, type, value, bigEndian);
    payload.insert(payload.end(), tiff.begin(), tiff.end());
    return payload;
}

std::vector<uint8_t> XmpPayload(const std::string& xmpBody) {
    std::vector<uint8_t> payload;
    AppendBytes(payload, "http://ns.adobe.com/xap/1.0/\0", 29);
    AppendBytes(payload, xmpBody.c_str(), xmpBody.size());
    return payload;
}

constexpr uint16_t TAG_SIMPLE_RATING = 0x4746;
constexpr uint16_t TYPE_SHORT = 3;

} // namespace

// --- ParseXmpRating -------------------------------------------------------

TEST(RatingMetadataTest, XmpAttributeForm) {
    EXPECT_EQ(ParseXmpRating(R"(<rdf:Description xmp:Rating="4"/>)"), 4);
}

TEST(RatingMetadataTest, XmpElementForm) {
    EXPECT_EQ(ParseXmpRating("<xmp:Rating>2</xmp:Rating>"), 2);
}

TEST(RatingMetadataTest, XmpSingleQuotesAndSpacing) {
    EXPECT_EQ(ParseXmpRating("xmp:Rating = '5'"), 5);
}

TEST(RatingMetadataTest, XmpElementFormWithWhitespace) {
    EXPECT_EQ(ParseXmpRating("<xmp:Rating>\n   3\n</xmp:Rating>"), 3);
}

TEST(RatingMetadataTest, XmpRejectedValueIsRecognized) {
    // Adobe marks a rejected photo with -1; it must not read as a star count.
    EXPECT_EQ(ParseXmpRating(R"(xmp:Rating="-1")"), REJECTED);
}

TEST(RatingMetadataTest, XmpZeroMeansUnrated) {
    EXPECT_EQ(ParseXmpRating(R"(xmp:Rating="0")"), 0);
}

TEST(RatingMetadataTest, XmpOutOfRangeIsIgnored) {
    EXPECT_FALSE(ParseXmpRating(R"(xmp:Rating="99")").has_value());
    EXPECT_FALSE(ParseXmpRating(R"(xmp:Rating="-7")").has_value());
}

TEST(RatingMetadataTest, XmpAbsentOrMalformed) {
    EXPECT_FALSE(ParseXmpRating("").has_value());
    EXPECT_FALSE(ParseXmpRating("<rdf:Description dc:title=\"x\"/>").has_value());
    EXPECT_FALSE(ParseXmpRating("xmp:Rating=\"\"").has_value());
    EXPECT_FALSE(ParseXmpRating("xmp:Rating").has_value());
}

TEST(RatingMetadataTest, XmpLongerPropertyNameIsNotMistaken) {
    // MicrosoftPhoto:Rating is a 0-99 percent value living under another
    // namespace; a name that merely starts with xmp:Rating must not match.
    EXPECT_FALSE(ParseXmpRating(R"(xmp:RatingPercent="75")").has_value());
}

TEST(RatingMetadataTest, XmpSkipsMalformedOccurrenceAndTakesTheNext) {
    EXPECT_EQ(ParseXmpRating(R"(xmp:Rating="" ... xmp:Rating="3")"), 3);
}

// --- ParseJpegRating ------------------------------------------------------

TEST(RatingMetadataTest, JpegExifRatingLittleEndian) {
    const auto jpeg = MakeJpeg({ ExifPayload(TAG_SIMPLE_RATING, TYPE_SHORT, 4, false) });
    EXPECT_EQ(ParseJpegRating(jpeg), 4);
}

TEST(RatingMetadataTest, JpegExifRatingBigEndian) {
    const auto jpeg = MakeJpeg({ ExifPayload(TAG_SIMPLE_RATING, TYPE_SHORT, 5, true) });
    EXPECT_EQ(ParseJpegRating(jpeg), 5);
}

TEST(RatingMetadataTest, JpegXmpRatingWhenExifTagAbsent) {
    // Lightroom writes only xmp:Rating, so an Exif-only scan would miss it.
    const auto jpeg = MakeJpeg({ XmpPayload(R"(<rdf:Description xmp:Rating="3"/>)") });
    EXPECT_EQ(ParseJpegRating(jpeg), 3);
}

TEST(RatingMetadataTest, JpegExifWinsOverXmp) {
    const auto jpeg = MakeJpeg({
        ExifPayload(TAG_SIMPLE_RATING, TYPE_SHORT, 4, false),
        XmpPayload(R"(xmp:Rating="1")"),
    });
    EXPECT_EQ(ParseJpegRating(jpeg), 4);
}

TEST(RatingMetadataTest, JpegXmpFallbackWhenExifSegmentHasNoRating) {
    // An Exif block without the rating tag must not stop the XMP fallback.
    const auto jpeg = MakeJpeg({
        ExifPayload(0x010F /*Make*/, TYPE_SHORT, 1, false),
        XmpPayload(R"(xmp:Rating="2")"),
    });
    EXPECT_EQ(ParseJpegRating(jpeg), 2);
}

TEST(RatingMetadataTest, JpegWithoutRating) {
    const auto jpeg = MakeJpeg({});
    EXPECT_FALSE(ParseJpegRating(jpeg).has_value());
}

TEST(RatingMetadataTest, NonJpegInputIsRejected) {
    const std::vector<uint8_t> png{ 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    EXPECT_FALSE(ParseJpegRating(png).has_value());
    EXPECT_FALSE(ParseJpegRating(std::vector<uint8_t>{}).has_value());
}

TEST(RatingMetadataTest, TruncatedAndMalformedInputDoesNotCrash) {
    // A header-sized read can cut a segment in half; every prefix must be safe.
    const auto full = MakeJpeg({ ExifPayload(TAG_SIMPLE_RATING, TYPE_SHORT, 4, false) });
    for (size_t len = 0; len < full.size(); ++len) {
        std::vector<uint8_t> prefix(full.begin(), full.begin() + len);
        (void)ParseJpegRating(prefix); // must not crash or read out of bounds
    }

    std::vector<uint8_t> garbage{ 0xFF, 0xD8, 0xFF, 0xE1, 0x00, 0x01, 0xFF, 0xFF, 0x00 };
    (void)ParseJpegRating(garbage);

    // Segment length pointing far past the buffer.
    std::vector<uint8_t> overrun{ 0xFF, 0xD8, 0xFF, 0xE1, 0x7F, 0xFF, 'E', 'x' };
    EXPECT_FALSE(ParseJpegRating(overrun).has_value());
}

TEST(RatingMetadataTest, JpegExifRatingOutOfRangeIsIgnored) {
    const auto jpeg = MakeJpeg({ ExifPayload(TAG_SIMPLE_RATING, TYPE_SHORT, 42, false) });
    EXPECT_FALSE(ParseJpegRating(jpeg).has_value());
}

// --- ResolvePairRating ----------------------------------------------------

TEST(RatingMetadataTest, PairSidecarWinsOnConflict) {
    // The maintainer's ruling: the sidecar is the authoritative carrier.
    const Resolved r = ResolvePairRating(/*inFile*/ 2, /*sidecar*/ 4);
    EXPECT_EQ(r.stars, 4);
    EXPECT_EQ(r.source, Source::Sidecar);
    EXPECT_TRUE(r.conflict);
    EXPECT_EQ(r.otherStars, 2);
}

TEST(RatingMetadataTest, PairAgreeingSidesAreNotAConflict) {
    const Resolved r = ResolvePairRating(3, 3);
    EXPECT_EQ(r.stars, 3);
    EXPECT_EQ(r.source, Source::Sidecar);
    EXPECT_FALSE(r.conflict);
}

TEST(RatingMetadataTest, PairSingleSidedRatings) {
    const Resolved onlySidecar = ResolvePairRating(std::nullopt, 5);
    EXPECT_EQ(onlySidecar.stars, 5);
    EXPECT_EQ(onlySidecar.source, Source::Sidecar);
    EXPECT_FALSE(onlySidecar.conflict);

    const Resolved onlyInFile = ResolvePairRating(1, std::nullopt);
    EXPECT_EQ(onlyInFile.stars, 1);
    EXPECT_EQ(onlyInFile.source, Source::InFile);
    EXPECT_FALSE(onlyInFile.conflict);
}

TEST(RatingMetadataTest, PairWithNoRatingAnywhere) {
    const Resolved r = ResolvePairRating(std::nullopt, std::nullopt);
    EXPECT_EQ(r.stars, 0);
    EXPECT_EQ(r.source, Source::None);
    EXPECT_FALSE(r.conflict);
}

TEST(RatingMetadataTest, PairRejectedIsDisplayedAsZeroStarsButStillConflicts) {
    const Resolved r = ResolvePairRating(/*inFile*/ 4, /*sidecar*/ REJECTED);
    EXPECT_EQ(r.stars, 0);
    EXPECT_EQ(r.source, Source::Sidecar);
    EXPECT_TRUE(r.conflict);
    EXPECT_EQ(r.otherStars, 4);
}
