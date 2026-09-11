#include <gtest/gtest.h>
#include "RatingMetadata.h"
#include "RatingWriter.h"
#include "RatingStore.h"
#include <windows.h>
#include <jxl/decode.h>
#include <jxl/decode_cxx.h>

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

// --- UpdateXmpRating -------------------------------------------------------

namespace {

// Shaped like a real Lightroom sidecar: the rating sits among develop
// settings that an update must not disturb.
const char* const LIGHTROOM_SIDECAR =
    "<?xpacket begin=\"\" id=\"W5M0MpCehiHzreSzNTczkc9d\"?>\n"
    "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\">\n"
    " <rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">\n"
    "  <rdf:Description rdf:about=\"\"\n"
    "    xmlns:xmp=\"http://ns.adobe.com/xap/1.0/\"\n"
    "    xmlns:crs=\"http://ns.adobe.com/camera-raw-settings/1.0/\"\n"
    "    xmp:Rating=\"4\"\n"
    "    crs:Exposure2012=\"+0.35\"\n"
    "    crs:Contrast2012=\"+12\"/>\n"
    " </rdf:RDF>\n"
    "</x:xmpmeta>\n"
    "<?xpacket end=\"w\"?>\n";

} // namespace

TEST(RatingMetadataTest, XmpUpdateKeepsDevelopSettings) {
    const auto out = UpdateXmpRating(LIGHTROOM_SIDECAR, 2);
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(ParseXmpRating(*out), 2);
    // Everything the photographer actually cares about must survive verbatim.
    EXPECT_NE(out->find("crs:Exposure2012=\"+0.35\""), std::string::npos);
    EXPECT_NE(out->find("crs:Contrast2012=\"+12\""), std::string::npos);
    EXPECT_NE(out->find("xmlns:crs="), std::string::npos);
    EXPECT_NE(out->find("<?xpacket end=\"w\"?>"), std::string::npos);
}

TEST(RatingMetadataTest, XmpUpdateRewritesOnlyTheRating) {
    const auto out = UpdateXmpRating(LIGHTROOM_SIDECAR, 5);
    ASSERT_TRUE(out.has_value());
    const std::string before(LIGHTROOM_SIDECAR);
    // The documents differ by one character: the rating digit.
    EXPECT_EQ(out->size(), before.size());
    size_t differing = 0;
    for (size_t i = 0; i < before.size(); ++i) {
        if ((*out)[i] != before[i]) ++differing;
    }
    EXPECT_EQ(differing, 1u);
}

TEST(RatingMetadataTest, XmpUpdateClearingRemovesTheProperty) {
    const auto out = UpdateXmpRating(LIGHTROOM_SIDECAR, 0);
    ASSERT_TRUE(out.has_value());
    EXPECT_FALSE(ParseXmpRating(*out).has_value());
    EXPECT_EQ(out->find("xmp:Rating"), std::string::npos);
    EXPECT_NE(out->find("crs:Exposure2012=\"+0.35\""), std::string::npos);
}

TEST(RatingMetadataTest, XmpUpdateInsertsWhenAbsent) {
    const std::string noRating =
        "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\">\n"
        " <rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">\n"
        "  <rdf:Description rdf:about=\"\" crs:Contrast2012=\"+12\"/>\n"
        " </rdf:RDF>\n"
        "</x:xmpmeta>\n";
    const auto out = UpdateXmpRating(noRating, 3);
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(ParseXmpRating(*out), 3);
    EXPECT_NE(out->find("crs:Contrast2012=\"+12\""), std::string::npos);
}

TEST(RatingMetadataTest, XmpUpdateHandlesElementForm) {
    const std::string elementForm =
        "<rdf:Description rdf:about=\"\">\n"
        "  <xmp:Rating>1</xmp:Rating>\n"
        "  <dc:title>keep me</dc:title>\n"
        "</rdf:Description>\n";
    const auto out = UpdateXmpRating(elementForm, 4);
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(ParseXmpRating(*out), 4);
    EXPECT_NE(out->find("<dc:title>keep me</dc:title>"), std::string::npos);
}

TEST(RatingMetadataTest, XmpUpdateRefusesUnfamiliarDocument) {
    // No rdf:Description to attach to: refusing is the only safe answer, since
    // the alternative is overwriting a file we do not understand.
    EXPECT_FALSE(UpdateXmpRating("just some text", 3).has_value());
    EXPECT_FALSE(UpdateXmpRating("", 3).has_value());
}

TEST(RatingMetadataTest, XmpUpdateClearingAnUnratedDocumentIsANoOp) {
    const std::string doc = "<rdf:Description rdf:about=\"\" crs:Contrast2012=\"+12\"/>";
    const auto out = UpdateXmpRating(doc, 0);
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(*out, doc);
}

TEST(RatingMetadataTest, MinimalSidecarRoundTrips) {
    for (int stars = 1; stars <= MAX_STARS; ++stars) {
        const std::string xmp = BuildMinimalXmp(stars);
        EXPECT_EQ(ParseXmpRating(xmp), stars);
    }
}

TEST(RatingMetadataTest, JpegShortApp1PayloadDoesNotUnderflow) {
    // SOI + APP1 marker + segLength=5 (payloadLen=3 < 6) + "Exif\0" prefix match
    const uint8_t shortPayload[] = {
        0xFF, 0xD8,             // SOI
        0xFF, 0xE1,             // APP1
        0x00, 0x05,             // length = 5 (payloadLen = 3)
        'E', 'x', 'i', 'f', 0, 0 // bytes matching "Exif\0\0" partially across boundaries
    };
    EXPECT_FALSE(ParseJpegRating(shortPayload).has_value());
}

namespace {

void AppendPngChunk(std::vector<uint8_t>& out, const char type[4], const std::vector<uint8_t>& data) {
    const uint32_t len = static_cast<uint32_t>(data.size());
    out.push_back(static_cast<uint8_t>(len >> 24));
    out.push_back(static_cast<uint8_t>(len >> 16));
    out.push_back(static_cast<uint8_t>(len >> 8));
    out.push_back(static_cast<uint8_t>(len & 0xFF));
    out.push_back(static_cast<uint8_t>(type[0]));
    out.push_back(static_cast<uint8_t>(type[1]));
    out.push_back(static_cast<uint8_t>(type[2]));
    out.push_back(static_cast<uint8_t>(type[3]));
    out.insert(out.end(), data.begin(), data.end());
    // 4-byte CRC dummy
    out.push_back(0); out.push_back(0); out.push_back(0); out.push_back(0);
}

std::vector<uint8_t> MakeBasePng() {
    std::vector<uint8_t> png = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
    // IHDR: 13 bytes
    std::vector<uint8_t> ihdr(13, 0);
    AppendPngChunk(png, "IHDR", ihdr);
    return png;
}

std::vector<uint8_t> MakeITXtXmpPayload(std::string_view xmp) {
    std::vector<uint8_t> payload;
    const std::string_view kw = "XML:com.adobe.xmp";
    payload.insert(payload.end(), kw.begin(), kw.end());
    payload.push_back(0); // keyword null-terminator
    payload.push_back(0); // compression flag = uncompressed
    payload.push_back(0); // compression method = 0
    payload.push_back(0); // language tag null-terminator
    payload.push_back(0); // translated keyword null-terminator
    payload.insert(payload.end(), xmp.begin(), xmp.end());
    return payload;
}

} // namespace

TEST(RatingMetadataTest, PngInvalidSignatureReturnsNullopt) {
    const uint8_t garbage[] = { 'N', 'O', 'T', 'P', 'N', 'G', '!', '!' };
    EXPECT_FALSE(ParsePngRating(garbage).has_value());
}

TEST(RatingMetadataTest, PngWithExifChunkReadsRating) {
    auto png = MakeBasePng();
    const auto tiff = MakeTiff(0x4746, 3, 4, /*bigEndian=*/false);
    AppendPngChunk(png, "eXIf", tiff);
    AppendPngChunk(png, "IDAT", {});
    AppendPngChunk(png, "IEND", {});

    EXPECT_EQ(ParsePngRating(png), 4);
}

TEST(RatingMetadataTest, PngWithITXtChunkReadsRating) {
    auto png = MakeBasePng();
    const std::string xmp = "<xmp:Rating>5</xmp:Rating>";
    AppendPngChunk(png, "iTXt", MakeITXtXmpPayload(xmp));
    AppendPngChunk(png, "IDAT", {});
    AppendPngChunk(png, "IEND", {});

    EXPECT_EQ(ParsePngRating(png), 5);
}

TEST(RatingMetadataTest, PngWithBothChunksPrefersExif) {
    auto png = MakeBasePng();
    const auto tiff = MakeTiff(0x4746, 3, 3, /*bigEndian=*/false);
    AppendPngChunk(png, "eXIf", tiff);

    const std::string xmp = "<xmp:Rating>2</xmp:Rating>";
    AppendPngChunk(png, "iTXt", MakeITXtXmpPayload(xmp));

    AppendPngChunk(png, "IDAT", {});
    AppendPngChunk(png, "IEND", {});

    EXPECT_EQ(ParsePngRating(png), 3);
}

TEST(RatingMetadataTest, PngStopsScanningAtIdat) {
    auto png = MakeBasePng();
    AppendPngChunk(png, "IDAT", {});
    // Metadata placed after IDAT should be ignored for fast hot-path scanning
    const auto tiff = MakeTiff(0x4746, 3, 4, /*bigEndian=*/false);
    AppendPngChunk(png, "eXIf", tiff);
    AppendPngChunk(png, "IEND", {});

    EXPECT_FALSE(ParsePngRating(png).has_value());
}

TEST(RatingMetadataTest, PngTruncatedBufferHandledSafely) {
    auto png = MakeBasePng();
    const auto tiff = MakeTiff(0x4746, 3, 4, /*bigEndian=*/false);
    AppendPngChunk(png, "eXIf", tiff);

    // Truncate in the middle of eXIf chunk
    png.resize(png.size() - 5);
    EXPECT_FALSE(ParsePngRating(png).has_value());
}

namespace {

void AppendWebpChunk(std::vector<uint8_t>& out, const char fourcc[4], const std::vector<uint8_t>& data) {
    out.push_back(static_cast<uint8_t>(fourcc[0]));
    out.push_back(static_cast<uint8_t>(fourcc[1]));
    out.push_back(static_cast<uint8_t>(fourcc[2]));
    out.push_back(static_cast<uint8_t>(fourcc[3]));
    const uint32_t len = static_cast<uint32_t>(data.size());
    out.push_back(static_cast<uint8_t>(len & 0xFF));
    out.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>((len >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((len >> 24) & 0xFF));
    out.insert(out.end(), data.begin(), data.end());
    if (len & 1) {
        out.push_back(0); // 1-byte padding for odd chunk size in RIFF
    }
}

std::vector<uint8_t> MakeBaseWebp() {
    std::vector<uint8_t> webp = {
        'R', 'I', 'F', 'F',
        0, 0, 0, 0, // Placeholder for total file size - 8
        'W', 'E', 'B', 'P'
    };
    return webp;
}

void FinalizeWebpSize(std::vector<uint8_t>& webp) {
    if (webp.size() >= 8) {
        const uint32_t riffSize = static_cast<uint32_t>(webp.size() - 8);
        webp[4] = static_cast<uint8_t>(riffSize & 0xFF);
        webp[5] = static_cast<uint8_t>((riffSize >> 8) & 0xFF);
        webp[6] = static_cast<uint8_t>((riffSize >> 16) & 0xFF);
        webp[7] = static_cast<uint8_t>((riffSize >> 24) & 0xFF);
    }
}

} // namespace

TEST(RatingMetadataTest, WebpInvalidSignatureReturnsNullopt) {
    const uint8_t notWebp[] = { 'R', 'I', 'F', 'F', 10, 0, 0, 0, 'A', 'V', 'I', ' ' };
    EXPECT_FALSE(ParseWebpRating(notWebp).has_value());
}

TEST(RatingMetadataTest, WebpWithExifChunkReadsRating) {
    auto webp = MakeBaseWebp();
    // Dummy VP8 chunk
    AppendWebpChunk(webp, "VP8 ", { 0, 1, 2, 3 });
    // EXIF chunk with raw TIFF
    const auto tiff = MakeTiff(0x4746, 3, 4, /*bigEndian=*/false);
    AppendWebpChunk(webp, "EXIF", tiff);
    FinalizeWebpSize(webp);

    EXPECT_EQ(ParseWebpRating(webp), 4);
}

TEST(RatingMetadataTest, WebpWithExifHeaderPrefixReadsRating) {
    auto webp = MakeBaseWebp();
    const auto tiff = MakeTiff(0x4746, 3, 5, /*bigEndian=*/false);
    std::vector<uint8_t> exifPayload = { 'E', 'x', 'i', 'f', 0, 0 };
    exifPayload.insert(exifPayload.end(), tiff.begin(), tiff.end());
    AppendWebpChunk(webp, "EXIF", exifPayload);
    FinalizeWebpSize(webp);

    EXPECT_EQ(ParseWebpRating(webp), 5);
}

TEST(RatingMetadataTest, WebpWithXmpChunkReadsRating) {
    auto webp = MakeBaseWebp();
    AppendWebpChunk(webp, "VP8 ", { 0, 1, 2, 3 });
    const std::string xmp = "<x:xmpmeta><xmp:Rating>3</xmp:Rating></x:xmpmeta>";
    std::vector<uint8_t> xmpData(xmp.begin(), xmp.end());
    AppendWebpChunk(webp, "XMP ", xmpData);
    FinalizeWebpSize(webp);

    EXPECT_EQ(ParseWebpRating(webp), 3);
}

TEST(RatingMetadataTest, WebpWithBothChunksPrefersExif) {
    auto webp = MakeBaseWebp();
    const auto tiff = MakeTiff(0x4746, 3, 2, /*bigEndian=*/false);
    AppendWebpChunk(webp, "EXIF", tiff);

    const std::string xmp = "<xmp:Rating>5</xmp:Rating>";
    std::vector<uint8_t> xmpData(xmp.begin(), xmp.end());
    AppendWebpChunk(webp, "XMP ", xmpData);
    FinalizeWebpSize(webp);

    EXPECT_EQ(ParseWebpRating(webp), 2);
}

TEST(RatingMetadataTest, WebpOddChunkPaddingHandledCorrectly) {
    auto webp = MakeBaseWebp();
    // Odd size chunk (3 bytes)
    AppendWebpChunk(webp, "VP8 ", { 0xAA, 0xBB, 0xCC });
    const auto tiff = MakeTiff(0x4746, 3, 1, /*bigEndian=*/false);
    AppendWebpChunk(webp, "EXIF", tiff);
    FinalizeWebpSize(webp);

    EXPECT_EQ(ParseWebpRating(webp), 1);
}

TEST(RatingMetadataTest, WebpTruncatedBufferHandledSafely) {
    auto webp = MakeBaseWebp();
    const auto tiff = MakeTiff(0x4746, 3, 4, /*bigEndian=*/false);
    AppendWebpChunk(webp, "EXIF", tiff);
    FinalizeWebpSize(webp);

    webp.resize(webp.size() - 4);
    EXPECT_FALSE(ParseWebpRating(webp).has_value());
}

TEST(RatingMetadataTest, WebpWriteAndReadbackRoundTrip) {
    auto webp = MakeBaseWebp();
    // A minimal VP8 chunk
    AppendWebpChunk(webp, "VP8 ", { 0x10, 0x20, 0x30, 0x40 });
    FinalizeWebpSize(webp);

    const std::wstring testPath = L"test_roundtrip.webp";
    DeleteFileW(testPath.c_str());

    HANDLE f = CreateFileW(testPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_NE(f, INVALID_HANDLE_VALUE);
    DWORD written = 0;
    WriteFile(f, webp.data(), static_cast<DWORD>(webp.size()), &written, nullptr);
    CloseHandle(f);

    // 1. Initial write of 4 stars
    const auto status1 = WriteRatingToImage(testPath, 4, /*allowTranscode=*/true);
    EXPECT_EQ(status1, WriteStatus::WrittenTranscode);

    // Read back and verify 4 stars
    {
        HANDLE rf = CreateFileW(testPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        ASSERT_NE(rf, INVALID_HANDLE_VALUE);
        std::vector<uint8_t> buf(64 * 1024);
        DWORD r = 0;
        ReadFile(rf, buf.data(), static_cast<DWORD>(buf.size()), &r, nullptr);
        CloseHandle(rf);
        buf.resize(r);
        EXPECT_EQ(ParseWebpRating(buf), 4);
    }

    // 2. Update to 5 stars
    const auto status2 = WriteRatingToImage(testPath, 5, /*allowTranscode=*/true);
    EXPECT_EQ(status2, WriteStatus::WrittenTranscode);

    {
        HANDLE rf = CreateFileW(testPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        ASSERT_NE(rf, INVALID_HANDLE_VALUE);
        std::vector<uint8_t> buf(64 * 1024);
        DWORD r = 0;
        ReadFile(rf, buf.data(), static_cast<DWORD>(buf.size()), &r, nullptr);
        CloseHandle(rf);
        buf.resize(r);
        EXPECT_EQ(ParseWebpRating(buf), 5);
    }

    // 3. Clear rating (0 stars)
    const auto status3 = WriteRatingToImage(testPath, 0, /*allowTranscode=*/true);
    EXPECT_EQ(status3, WriteStatus::WrittenTranscode);

    {
        HANDLE rf = CreateFileW(testPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        ASSERT_NE(rf, INVALID_HANDLE_VALUE);
        std::vector<uint8_t> buf(64 * 1024);
        DWORD r = 0;
        ReadFile(rf, buf.data(), static_cast<DWORD>(buf.size()), &r, nullptr);
        CloseHandle(rf);
        buf.resize(r);
        EXPECT_FALSE(ParseWebpRating(buf).has_value());
    }

    DeleteFileW(testPath.c_str());
}

TEST(RatingMetadataTest, TestRealWebpWrite) {
    const wchar_t* srcPath = LR"(D:\Works\Personal\Coding\QuickView\Local-Files\test_img\zoltan-tasi-CLJeQCr2F_A-unsplash.webp)";
    const wchar_t* copyPath = L"test_real_copy.webp";
    CopyFileW(srcPath, copyPath, FALSE);

    // Initial check: no rating
    EXPECT_FALSE(RatingStore::ReadRatingFromFile(copyPath).has_value());

    // 1. Write 4 stars
    const auto status1 = WriteRatingToImage(copyPath, 4, /*allowTranscode=*/true);
    EXPECT_EQ(status1, WriteStatus::WrittenTranscode);
    EXPECT_EQ(RatingStore::ReadRatingFromFile(copyPath), 4);

    // 2. Update to 5 stars
    const auto status2 = WriteRatingToImage(copyPath, 5, /*allowTranscode=*/true);
    EXPECT_EQ(status2, WriteStatus::WrittenTranscode);
    EXPECT_EQ(RatingStore::ReadRatingFromFile(copyPath), 5);

    // 3. Clear rating (0 stars)
    const auto status3 = WriteRatingToImage(copyPath, 0, /*allowTranscode=*/true);
    EXPECT_EQ(status3, WriteStatus::WrittenTranscode);
    EXPECT_FALSE(RatingStore::ReadRatingFromFile(copyPath).has_value());

    DeleteFileW(copyPath);
}

TEST(RatingMetadataTest, IsobmffInvalidSignatureReturnsNullopt) {
    const std::vector<uint8_t> notIsobmff{ 'N', 'O', 'P', 'E', 0, 0, 0, 0 };
    EXPECT_FALSE(ParseIsobmffRating(notIsobmff).has_value());
}

static std::vector<uint8_t> MakeIsobmffWithXmp(const std::string& xmpStr) {
    auto writeU16 = [](std::vector<uint8_t>& buf, uint16_t val) {
        buf.push_back(static_cast<uint8_t>((val >> 8) & 0xFF));
        buf.push_back(static_cast<uint8_t>(val & 0xFF));
    };
    auto writeU32 = [](std::vector<uint8_t>& buf, uint32_t val) {
        buf.push_back(static_cast<uint8_t>((val >> 24) & 0xFF));
        buf.push_back(static_cast<uint8_t>((val >> 16) & 0xFF));
        buf.push_back(static_cast<uint8_t>((val >> 8) & 0xFF));
        buf.push_back(static_cast<uint8_t>(val & 0xFF));
    };

    std::vector<uint8_t> ftyp;
    writeU32(ftyp, 24);
    ftyp.insert(ftyp.end(), { 'f', 't', 'y', 'p', 'a', 'v', 'i', 'f', 0, 0, 0, 0, 'a', 'v', 'i', 'f', 'm', 'i', 'f', '1' });

    std::vector<uint8_t> hdlr;
    writeU32(hdlr, 33);
    hdlr.insert(hdlr.end(), { 'h', 'd', 'l', 'r', 0, 0, 0, 0, 0, 0, 0, 0, 'p', 'i', 'c', 't' });
    hdlr.insert(hdlr.end(), 12, 0);
    hdlr.push_back(0);

    const std::string mime = "application/rdf+xml";
    std::vector<uint8_t> infePayload;
    writeU32(infePayload, 2u << 24);
    writeU16(infePayload, 1);
    writeU16(infePayload, 0);
    infePayload.insert(infePayload.end(), { 'm', 'i', 'm', 'e', 0 });
    infePayload.insert(infePayload.end(), mime.begin(), mime.end());
    infePayload.push_back(0);

    std::vector<uint8_t> infe;
    writeU32(infe, static_cast<uint32_t>(8 + infePayload.size()));
    infe.insert(infe.end(), { 'i', 'n', 'f', 'e' });
    infe.insert(infe.end(), infePayload.begin(), infePayload.end());

    std::vector<uint8_t> iinf;
    writeU32(iinf, static_cast<uint32_t>(8 + 4 + 2 + infe.size()));
    iinf.insert(iinf.end(), { 'i', 'i', 'n', 'f', 0, 0, 0, 0 });
    writeU16(iinf, 1);
    iinf.insert(iinf.end(), infe.begin(), infe.end());

    const size_t metaBeforeIloc = 8 + 4 + hdlr.size() + iinf.size();
    const size_t ilocBoxLen = 8 + 4 + 1 + 1 + 2 + 14;
    const size_t metaTotalLen = metaBeforeIloc + ilocBoxLen;
    const uint32_t xmpAbsOffset = static_cast<uint32_t>(ftyp.size() + metaTotalLen + 8);

    std::vector<uint8_t> iloc;
    writeU32(iloc, static_cast<uint32_t>(ilocBoxLen));
    iloc.insert(iloc.end(), { 'i', 'l', 'o', 'c', 0, 0, 0, 0, 0x44, 0x00 });
    writeU16(iloc, 1);
    writeU16(iloc, 1);
    writeU16(iloc, 0);
    writeU16(iloc, 1);
    writeU32(iloc, xmpAbsOffset);
    writeU32(iloc, static_cast<uint32_t>(xmpStr.size()));

    std::vector<uint8_t> meta;
    writeU32(meta, static_cast<uint32_t>(metaTotalLen));
    meta.insert(meta.end(), { 'm', 'e', 't', 'a', 0, 0, 0, 0 });
    meta.insert(meta.end(), hdlr.begin(), hdlr.end());
    meta.insert(meta.end(), iinf.begin(), iinf.end());
    meta.insert(meta.end(), iloc.begin(), iloc.end());

    std::vector<uint8_t> mdat;
    writeU32(mdat, static_cast<uint32_t>(8 + xmpStr.size()));
    mdat.insert(mdat.end(), { 'm', 'd', 'a', 't' });
    mdat.insert(mdat.end(), xmpStr.begin(), xmpStr.end());

    std::vector<uint8_t> out;
    out.reserve(ftyp.size() + meta.size() + mdat.size());
    out.insert(out.end(), ftyp.begin(), ftyp.end());
    out.insert(out.end(), meta.begin(), meta.end());
    out.insert(out.end(), mdat.begin(), mdat.end());
    return out;
}

TEST(RatingMetadataTest, IsobmffSyntheticXmpReadsRating) {
    const std::string xml = "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\"><rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\"><rdf:Description xmp:Rating=\"4\"/></rdf:RDF></x:xmpmeta>";
    const auto data = MakeIsobmffWithXmp(xml);
    EXPECT_EQ(ParseIsobmffRating(data), 4);
}

TEST(RatingMetadataTest, IsobmffRealFileWithoutRatingReturnsNullopt) {
    const wchar_t* path = LR"(D:\Works\Personal\Coding\QuickView\Local-Files\test_img\avif\kimono.rotate90.avif)";
    HANDLE hFile = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (hFile != INVALID_HANDLE_VALUE) {
        std::vector<uint8_t> buf(64 * 1024);
        DWORD r = 0;
        ReadFile(hFile, buf.data(), static_cast<DWORD>(buf.size()), &r, nullptr);
        CloseHandle(hFile);
        buf.resize(r);
        EXPECT_FALSE(ParseIsobmffRating(buf).has_value());
    }
}

TEST(RatingMetadataTest, IsobmffWriteAndReadbackRoundTrip) {
    const wchar_t* copyPath = L"test_isobmff_synth_roundtrip.avif";
    // Build initial AVIF with 4 stars and 1024 bytes XMP padding
    std::string xml = "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\"><rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\"><rdf:Description xmp:Rating=\"4\"/></rdf:RDF></x:xmpmeta>";
    xml.append(1024, ' ');
    const auto data = MakeIsobmffWithXmp(xml);

    HANDLE hFile = CreateFileW(copyPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_NE(hFile, INVALID_HANDLE_VALUE);
    DWORD written = 0;
    WriteFile(hFile, data.data(), static_cast<DWORD>(data.size()), &written, nullptr);
    CloseHandle(hFile);

    // Initial check: rating is 4
    EXPECT_EQ(RatingStore::ReadRatingFromFile(copyPath), 4);

    // 1. Update to 5 stars
    const auto status1 = WriteRatingToImage(copyPath, 5, /*allowTranscode=*/true);
    EXPECT_EQ(status1, WriteStatus::WrittenInPlace);
    EXPECT_EQ(RatingStore::ReadRatingFromFile(copyPath), 5);

    // 2. Update to 3 stars
    const auto status2 = WriteRatingToImage(copyPath, 3, /*allowTranscode=*/true);
    EXPECT_EQ(status2, WriteStatus::WrittenInPlace);
    EXPECT_EQ(RatingStore::ReadRatingFromFile(copyPath), 3);

    // 3. Clear rating (0 stars)
    const auto status3 = WriteRatingToImage(copyPath, 0, /*allowTranscode=*/true);
    EXPECT_EQ(status3, WriteStatus::WrittenInPlace);
    EXPECT_FALSE(RatingStore::ReadRatingFromFile(copyPath).has_value());

    DeleteFileW(copyPath);
}

TEST(RatingMetadataTest, IsobmffRealHeifWriteAndReadback) {
    const wchar_t* srcPath = LR"(D:\Works\Personal\Coding\QuickView\Local-Files\test_img\格式测试\sample1.heif)";
    const wchar_t* copyPath = L"test_real_sample1.heif";
    CopyFileW(srcPath, copyPath, FALSE);

    const auto status1 = WriteRatingToImage(copyPath, 4, /*allowTranscode=*/true);
    printf("\nReal HEIF WriteStatus: %d\n", static_cast<int>(status1));
    EXPECT_EQ(status1, WriteStatus::WrittenInPlace);
    EXPECT_EQ(RatingStore::ReadRatingFromFile(copyPath), 4);
    DeleteFileW(copyPath);
}

TEST(RatingMetadataTest, IsobmffRealAvifWithoutXmpInjectAndReadback) {
    const wchar_t* srcPath = LR"(D:\Works\Personal\Coding\QuickView\Local-Files\test_img\avif\kimono.rotate90.avif)";
    const wchar_t* copyPath = L"test_kimono_inject_test.avif";
    CopyFileW(srcPath, copyPath, FALSE);

    // Initial check: no rating
    EXPECT_FALSE(RatingStore::ReadRatingFromFile(copyPath).has_value());

    // 1. Inject 4 stars into a file without preexisting XMP
    const auto status1 = WriteRatingToImage(copyPath, 4, /*allowTranscode=*/true);
    EXPECT_EQ(status1, WriteStatus::WrittenInPlace);
    EXPECT_EQ(RatingStore::ReadRatingFromFile(copyPath), 4);

    // 2. In-place update the newly injected XMP to 5 stars
    const auto status2 = WriteRatingToImage(copyPath, 5, /*allowTranscode=*/true);
    EXPECT_EQ(status2, WriteStatus::WrittenInPlace);
    EXPECT_EQ(RatingStore::ReadRatingFromFile(copyPath), 5);

    DeleteFileW(copyPath);
}

TEST(RatingMetadataTest, IsobmffRealAvifYuv444pExifInPlacePatch) {
    const wchar_t* srcPath = LR"(D:\Works\Personal\Coding\QuickView\Local-Files\test_img\avif\avif-yuv444p.avif)";
    const wchar_t* copyPath = L"test_yuv444p_inplace_test.avif";
    CopyFileW(srcPath, copyPath, FALSE);

    // Initial check: clean file has no rating
    EXPECT_FALSE(RatingStore::ReadRatingFromFile(copyPath).has_value());

    // 1. In-place update Exif rating to 5 stars
    const auto status1 = WriteRatingToImage(copyPath, 5, /*allowTranscode=*/true);
    EXPECT_EQ(status1, WriteStatus::WrittenInPlace);
    EXPECT_EQ(RatingStore::ReadRatingFromFile(copyPath), 5);

    // 2. In-place update Exif rating to 3 stars
    const auto status2 = WriteRatingToImage(copyPath, 3, /*allowTranscode=*/true);
    EXPECT_EQ(status2, WriteStatus::WrittenInPlace);
    EXPECT_EQ(RatingStore::ReadRatingFromFile(copyPath), 3);

    // 3. Clear rating (0 stars)
    const auto status3 = WriteRatingToImage(copyPath, 0, /*allowTranscode=*/true);
    EXPECT_EQ(status3, WriteStatus::WrittenInPlace);
    const auto res = RatingStore::ReadRatingFromFile(copyPath);
    EXPECT_TRUE(!res.has_value() || *res == 0);

    DeleteFileW(copyPath);
}

TEST(RatingMetadataTest, PatchXmpRatingInPlaceStrictSuccess) {
    // Document with 100 bytes of padding
    const std::string xmp =
        "<?xpacket begin=\"\xEF\xBB\xBF\" id=\"W5M0MpCehiHzreSzNTczkc9d\"?>\n"
        "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\">\n"
        " <rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">\n"
        "  <rdf:Description rdf:about=\"\"/>\n"
        " </rdf:RDF>\n"
        "</x:xmpmeta>\n"
        "                                                                                                    "
        "<?xpacket end=\"w\"?>";

    const auto patched = PatchXmpRatingInPlaceStrict(xmp, 4);
    ASSERT_TRUE(patched.has_value());
    EXPECT_EQ(patched->size(), xmp.size());
    EXPECT_EQ(ParseXmpRating(*patched), 4);

    // Modify existing property
    const auto modified = PatchXmpRatingInPlaceStrict(*patched, 5);
    ASSERT_TRUE(modified.has_value());
    EXPECT_EQ(modified->size(), xmp.size());
    EXPECT_EQ(ParseXmpRating(*modified), 5);

    // Clear property (0 stars)
    const auto cleared = PatchXmpRatingInPlaceStrict(*modified, 0);
    ASSERT_TRUE(cleared.has_value());
    EXPECT_EQ(cleared->size(), xmp.size());
    EXPECT_FALSE(ParseXmpRating(*cleared).has_value());
}

TEST(RatingMetadataTest, PatchXmpRatingInPlaceStrictInsufficientPadding) {
    // Document with zero padding
    const std::string xmpNoPad =
        "<?xpacket begin=\"\xEF\xBB\xBF\" id=\"W5M0MpCehiHzreSzNTczkc9d\"?>\n"
        "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\">\n"
        " <rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">\n"
        "  <rdf:Description rdf:about=\"\"/>\n"
        " </rdf:RDF>\n"
        "</x:xmpmeta>\n"
        "<?xpacket end=\"w\"?>";

    EXPECT_FALSE(PatchXmpRatingInPlaceStrict(xmpNoPad, 4).has_value());
}

TEST(RatingMetadataTest, UserReportedCasesRegressionTest) {
    // 1. Chef with trumpet
    const wchar_t* chefOrig = LR"(D:\Works\Personal\Coding\QuickView\Local-Files\test_img\Hdr\chef-with-trumpet_110407.heic)";
    const wchar_t* chefTest = L"test_chef_regression.heic";
    CopyFileW(chefOrig, chefTest, FALSE);
    EXPECT_EQ(WriteRatingToImage(chefTest, 4, true), WriteStatus::WrittenInPlace);
    EXPECT_EQ(RatingStore::ReadRatingFromFile(chefTest), 4);

    // 2. Childrens show theater
    const wchar_t* childOrig = LR"(D:\Works\Personal\Coding\QuickView\Local-Files\test_img\Hdr\childrens-show-theater.heic)";
    const wchar_t* childTest = L"test_children_regression.heic";
    CopyFileW(childOrig, childTest, FALSE);
    EXPECT_EQ(WriteRatingToImage(childTest, 4, true), WriteStatus::WrittenInPlace);
    EXPECT_EQ(RatingStore::ReadRatingFromFile(childTest), 4);

    // 3. AVIF
    const wchar_t* avifOrig = LR"(D:\Works\Personal\Coding\QuickView\Local-Files\test_img\avif\test_webp_lossy.avif)";
    const wchar_t* avifTest = L"test_avif_regression.avif";
    CopyFileW(avifOrig, avifTest, FALSE);
    EXPECT_EQ(WriteRatingToImage(avifTest, 4, true), WriteStatus::WrittenInPlace);
    EXPECT_EQ(RatingStore::ReadRatingFromFile(avifTest), 4);

    // 4. DJI Drone HEIC with multiple XMP items (primary item 39 vs telemetry items 77, 78, 79)
    const wchar_t* djiOrig = LR"(D:\Works\Personal\Coding\QuickView\Local-Files\test_img\Hdr\DJI_1_0927_D_110407.heic)";
    const wchar_t* djiTest = L"test_dji_regression.heic";
    CopyFileW(djiOrig, djiTest, FALSE);
    EXPECT_EQ(WriteRatingToImage(djiTest, 5, true), WriteStatus::WrittenInPlace);
    EXPECT_EQ(RatingStore::ReadRatingFromFile(djiTest), 5);

    DeleteFileW(chefTest);
    DeleteFileW(childTest);
    DeleteFileW(avifTest);
    DeleteFileW(djiTest);
}

namespace {

bool VerifyJxlDecodeBasic(const std::wstring& path, uint32_t* outW = nullptr, uint32_t* outH = nullptr) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(file, &sz) || sz.QuadPart == 0 || sz.QuadPart > 100 * 1024 * 1024) {
        CloseHandle(file);
        return false;
    }
    std::vector<uint8_t> data(static_cast<size_t>(sz.QuadPart));
    DWORD read = 0;
    ReadFile(file, data.data(), static_cast<DWORD>(data.size()), &read, nullptr);
    CloseHandle(file);

    auto dec = JxlDecoderMake(nullptr);
    if (JXL_DEC_SUCCESS != JxlDecoderSubscribeEvents(dec.get(), JXL_DEC_BASIC_INFO)) {
        return false;
    }
    JxlDecoderSetInput(dec.get(), data.data(), data.size());
    for (;;) {
        JxlDecoderStatus st = JxlDecoderProcessInput(dec.get());
        if (st == JXL_DEC_ERROR || st == JXL_DEC_NEED_MORE_INPUT) return false;
        if (st == JXL_DEC_BASIC_INFO) {
            JxlBasicInfo info{};
            if (JXL_DEC_SUCCESS == JxlDecoderGetBasicInfo(dec.get(), &info)) {
                if (outW) *outW = info.xsize;
                if (outH) *outH = info.ysize;
                return true;
            }
            return false;
        }
        if (st == JXL_DEC_SUCCESS) break;
    }
    return false;
}

} // namespace

TEST(RatingMetadataTest, JxlInvalidSignatureReturnsNullopt) {
    std::vector<uint8_t> garbage{ 0x00, 0x00, 0x00, 0x0C, 'F', 'A', 'K', 'E', 0x0D, 0x0A, 0x87, 0x0A };
    EXPECT_FALSE(ParseJxlRating(garbage).has_value());
    EXPECT_FALSE(ParseJxlRating({}).has_value());
}

TEST(RatingMetadataTest, JxlBareCodestreamWithoutRatingReturnsNullopt) {
    std::vector<uint8_t> bare{ 0xFF, 0x0A, 0x00, 0x01, 0x02, 0x03 };
    EXPECT_FALSE(ParseJxlRating(bare).has_value());
}

TEST(RatingMetadataTest, JxlSyntheticExifContainerReadsRating) {
    std::vector<uint8_t> c;
    static constexpr uint8_t JXL_SIG[12] = {
        0x00, 0x00, 0x00, 0x0C, 'J', 'X', 'L', ' ', 0x0D, 0x0A, 0x87, 0x0A
    };
    c.insert(c.end(), JXL_SIG, JXL_SIG + 12);

    static constexpr uint8_t JXL_FTYP[20] = {
        0x00, 0x00, 0x00, 0x14, 'f', 't', 'y', 'p',
        'j', 'x', 'l', ' ', 0x00, 0x00, 0x00, 0x00,
        'j', 'x', 'l', ' '
    };
    c.insert(c.end(), JXL_FTYP, JXL_FTYP + 20);

    const auto tiff = MakeTiff(0x4746, 3, 4, false); // 4 stars, LE
    const uint32_t exifBoxSize = 8 + 4 + static_cast<uint32_t>(tiff.size());
    c.push_back(static_cast<uint8_t>((exifBoxSize >> 24) & 0xFF));
    c.push_back(static_cast<uint8_t>((exifBoxSize >> 16) & 0xFF));
    c.push_back(static_cast<uint8_t>((exifBoxSize >> 8) & 0xFF));
    c.push_back(static_cast<uint8_t>(exifBoxSize & 0xFF));
    c.push_back('E'); c.push_back('x'); c.push_back('i'); c.push_back('f');
    c.push_back(0); c.push_back(0); c.push_back(0); c.push_back(0);
    c.insert(c.end(), tiff.begin(), tiff.end());

    EXPECT_EQ(ParseJxlRating(c), 4);
}

TEST(RatingMetadataTest, JxlSyntheticXmlContainerReadsRating) {
    std::vector<uint8_t> c;
    static constexpr uint8_t JXL_SIG[12] = {
        0x00, 0x00, 0x00, 0x0C, 'J', 'X', 'L', ' ', 0x0D, 0x0A, 0x87, 0x0A
    };
    c.insert(c.end(), JXL_SIG, JXL_SIG + 12);

    const std::string xmp = "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\"><rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\"><rdf:Description xmlns:xmp=\"http://ns.adobe.com/xap/1.0/\" xmp:Rating=\"3\"/></rdf:RDF></x:xmpmeta>";
    const uint32_t xmlBoxSize = 8 + static_cast<uint32_t>(xmp.size());
    c.push_back(static_cast<uint8_t>((xmlBoxSize >> 24) & 0xFF));
    c.push_back(static_cast<uint8_t>((xmlBoxSize >> 16) & 0xFF));
    c.push_back(static_cast<uint8_t>((xmlBoxSize >> 8) & 0xFF));
    c.push_back(static_cast<uint8_t>(xmlBoxSize & 0xFF));
    c.push_back('x'); c.push_back('m'); c.push_back('l'); c.push_back(' ');
    c.insert(c.end(), xmp.begin(), xmp.end());

    EXPECT_EQ(ParseJxlRating(c), 3);
}

TEST(RatingMetadataTest, JxlBareCodestreamWrapAndReadback) {
    const wchar_t* srcPath = LR"(D:\Works\Personal\Coding\QuickView\Local-Files\test_img\Hdr\sunrise-PQ.jxl)";
    const wchar_t* copyPath = L"test_sunrise_wrap_test.jxl";
    CopyFileW(srcPath, copyPath, FALSE);

    // Initial check: bare codestream has no rating
    EXPECT_FALSE(RatingStore::ReadRatingFromFile(copyPath).has_value());

    // 1. Wrap into container and write 5 stars (returns WrittenTranscode)
    const auto st1 = WriteRatingToImage(copyPath, 5, /*allowTranscode=*/true);
    EXPECT_EQ(st1, WriteStatus::WrittenTranscode);
    EXPECT_EQ(RatingStore::ReadRatingFromFile(copyPath), 5);

    // Verify image decodes properly with exact dimensions
    uint32_t w = 0, h = 0;
    EXPECT_TRUE(VerifyJxlDecodeBasic(copyPath, &w, &h));
    EXPECT_EQ(w, 944);
    EXPECT_EQ(h, 944);

    // 2. In-place update rating to 2 stars (container already has Exif box)
    const auto st2 = WriteRatingToImage(copyPath, 2, /*allowTranscode=*/true);
    EXPECT_EQ(st2, WriteStatus::WrittenInPlace);
    EXPECT_EQ(RatingStore::ReadRatingFromFile(copyPath), 2);

    // 3. Clear rating (0 stars)
    const auto st3 = WriteRatingToImage(copyPath, 0, /*allowTranscode=*/true);
    EXPECT_EQ(st3, WriteStatus::WrittenInPlace);
    const auto r = RatingStore::ReadRatingFromFile(copyPath);
    EXPECT_TRUE(!r.has_value() || *r == 0);

    DeleteFileW(copyPath);
}

TEST(RatingMetadataTest, JxlRealContainerRebuildAndInPlace) {
    const wchar_t* srcPath = LR"(D:\Works\Personal\Coding\QuickView\Local-Files\test_img\zoltan-tasi-CLJeQCr2F_A-unsplash.jxl)";
    const wchar_t* copyPath = L"test_zoltan_rebuild_test.jxl";
    CopyFileW(srcPath, copyPath, FALSE);

    // 1. Write 4 stars into file with brob compressed metadata (rebuilds container)
    const auto st1 = WriteRatingToImage(copyPath, 4, /*allowTranscode=*/true);
    EXPECT_EQ(st1, WriteStatus::WrittenTranscode);
    EXPECT_EQ(RatingStore::ReadRatingFromFile(copyPath), 4);

    // Verify image decodes properly with exact dimensions
    uint32_t w = 0, h = 0;
    EXPECT_TRUE(VerifyJxlDecodeBasic(copyPath, &w, &h));
    EXPECT_GT(w, 0);
    EXPECT_GT(h, 0);

    // 2. Second write should succeed in-place!
    const auto st2 = WriteRatingToImage(copyPath, 3, /*allowTranscode=*/true);
    EXPECT_EQ(st2, WriteStatus::WrittenInPlace);
    EXPECT_EQ(RatingStore::ReadRatingFromFile(copyPath), 3);

    DeleteFileW(copyPath);
}

