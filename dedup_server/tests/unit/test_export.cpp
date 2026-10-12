// CHAIN-6 (docs/todo_tool_chain.md §8, §12): export writes a NEW file and
// nothing else — never overwrites, names in sequence, EXIF carried over
// upright and without the thumbnail, no sidecar beside the export, and an
// export dropped into a registered location is just one more file for the
// scan to find.
#include <gtest/gtest.h>
#include <libexif/exif-data.h>
#include <tiffio.h>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <unistd.h>
#include <sys/stat.h>
#include "adjustments/adjustment_document.hpp"
#include "adjustments/chain_engine.hpp"
#include "adjustments/export_models.hpp"
#include "adjustments/export_renderer.hpp"
#include "adjustments/tool_registry.hpp"
#include "filesmanager/file_scanner.hpp"

namespace fs = std::filesystem;
using namespace MediaDedup::Adjustments;

namespace
{
    std::vector<uint8_t> readAll(const std::string &path)
    {
        std::ifstream in(path, std::ios::binary);
        return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }

    void writeAll(const std::string &path, const std::vector<uint8_t> &bytes)
    {
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }

    // A camera-like EXIF block built with libexif itself: Make, Model,
    // Orientation 6 (rotate 90 CW), ExposureTime, a Software tag the
    // export must replace, and a thumbnail. Returned as the APP1 payload.
    std::vector<uint8_t> cameraExif(uint16_t orientation)
    {
        ExifData *ed = exif_data_new();
        exif_data_set_byte_order(ed, EXIF_BYTE_ORDER_INTEL);
        ExifMem *mem = exif_mem_new_default();
        auto add_ascii = [&](ExifIfd ifd, ExifTag tag, const char *text) {
            ExifEntry *e = exif_entry_new_mem(mem);
            e->tag = tag;
            e->format = EXIF_FORMAT_ASCII;
            e->components = std::strlen(text) + 1;
            e->size = static_cast<unsigned>(e->components);
            e->data = static_cast<unsigned char *>(exif_mem_alloc(mem, e->size));
            std::memcpy(e->data, text, e->size);
            exif_content_add_entry(ed->ifd[ifd], e);
            exif_entry_unref(e);
        };
        auto add_short = [&](ExifIfd ifd, ExifTag tag, uint16_t v) {
            ExifEntry *e = exif_entry_new_mem(mem);
            e->tag = tag;
            e->format = EXIF_FORMAT_SHORT;
            e->components = 1;
            e->size = 2;
            e->data = static_cast<unsigned char *>(exif_mem_alloc(mem, 2));
            exif_set_short(e->data, EXIF_BYTE_ORDER_INTEL, v);
            exif_content_add_entry(ed->ifd[ifd], e);
            exif_entry_unref(e);
        };
        auto add_rational = [&](ExifIfd ifd, ExifTag tag, uint32_t num, uint32_t den) {
            ExifEntry *e = exif_entry_new_mem(mem);
            e->tag = tag;
            e->format = EXIF_FORMAT_RATIONAL;
            e->components = 1;
            e->size = 8;
            e->data = static_cast<unsigned char *>(exif_mem_alloc(mem, 8));
            exif_set_rational(e->data, EXIF_BYTE_ORDER_INTEL, ExifRational{num, den});
            exif_content_add_entry(ed->ifd[ifd], e);
            exif_entry_unref(e);
        };
        add_ascii(EXIF_IFD_0, EXIF_TAG_MAKE, "Vurio Test Cameras");
        add_ascii(EXIF_IFD_0, EXIF_TAG_MODEL, "VT-1");
        add_ascii(EXIF_IFD_0, EXIF_TAG_SOFTWARE, "Camera Firmware 1.0");
        add_ascii(EXIF_IFD_0, EXIF_TAG_DATE_TIME, "2026:10:11 12:00:00");
        add_short(EXIF_IFD_0, EXIF_TAG_ORIENTATION, orientation);
        add_rational(EXIF_IFD_EXIF, EXIF_TAG_EXPOSURE_TIME, 1, 250);
        add_rational(EXIF_IFD_EXIF, EXIF_TAG_FNUMBER, 28, 10);
        add_short(EXIF_IFD_EXIF, EXIF_TAG_ISO_SPEED_RATINGS, 400);
        add_ascii(EXIF_IFD_EXIF, EXIF_TAG_DATE_TIME_ORIGINAL, "2026:10:11 12:00:00");
        add_rational(EXIF_IFD_GPS, static_cast<ExifTag>(EXIF_TAG_GPS_ALTITUDE), 1234, 10);
        // A tiny JPEG as the thumbnail.
        cv::Mat thumb(8, 8, CV_8UC3, cv::Scalar(10, 20, 30));
        std::vector<uchar> tjpg;
        cv::imencode(".jpg", thumb, tjpg);
        ed->data = static_cast<unsigned char *>(exif_mem_alloc(mem, static_cast<unsigned>(tjpg.size())));
        std::memcpy(ed->data, tjpg.data(), tjpg.size());
        ed->size = static_cast<unsigned>(tjpg.size());
        unsigned char *buf = nullptr;
        unsigned len = 0;
        exif_data_save_data(ed, &buf, &len);
        std::vector<uint8_t> out(buf, buf + len);
        free(buf);
        exif_mem_unref(mem);
        exif_data_unref(ed);
        return out;
    }

    // A 640x480 test picture (left half red, right half blue) with EXIF.
    std::vector<uint8_t> sourceJpeg(uint16_t orientation)
    {
        cv::Mat img(480, 640, CV_8UC3, cv::Scalar(0, 0, 200));
        img(cv::Rect(0, 0, 320, 480)) = cv::Scalar(200, 0, 0);
        std::vector<uchar> jpg;
        cv::imencode(".jpg", img, jpg, {cv::IMWRITE_JPEG_QUALITY, 95});
        return ExifTransplant::intoJpeg(std::vector<uint8_t>(jpg.begin(), jpg.end()), cameraExif(orientation));
    }

    struct Parsed
    {
        ExifData *ed = nullptr;
        explicit Parsed(const std::vector<uint8_t> &file)
        {
            ed = exif_data_new();
            exif_data_unset_option(ed, EXIF_DATA_OPTION_FOLLOW_SPECIFICATION);
            exif_data_load_data(ed, file.data(), static_cast<unsigned>(file.size()));
        }
        ~Parsed() { exif_data_unref(ed); }
        std::string text(ExifTag tag) const
        {
            ExifEntry *e = exif_data_get_entry(ed, tag);
            if (!e)
                return "";
            char buf[256] = {0};
            exif_entry_get_value(e, buf, sizeof buf);
            return buf;
        }
        bool has(ExifTag tag) const { return exif_data_get_entry(ed, tag) != nullptr; }
    };

    RenderPlan monochromePlan()
    {
        const char *json = R"({"format":"vurio-adjustments","version":2,"chain":[{"id":"n1","tool":"monochrome","version":1,"params":{}}]})";
        AdjustmentDocument doc;
        std::string error;
        if (!AdjustmentDocument::parse(json, doc, error))
            throw std::runtime_error("parse: " + error);
        RenderPlan plan;
        if (!doc.compile(ToolRegistry::instance(), plan, error))
            throw std::runtime_error("compile: " + error);
        return plan;
    }

    std::set<std::string> listing(const fs::path &folder)
    {
        std::set<std::string> names;
        for (const auto &entry : fs::directory_iterator(folder))
            names.insert(entry.path().filename().string());
        return names;
    }
}

class ExportTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        base_ = fs::temp_directory_path() / ("mds_export_" + std::to_string(::getpid()) + "_" + std::to_string(++counter_));
        fs::remove_all(base_);
        fs::create_directories(base_ / "lib");
        fs::create_directories(base_ / "out");
        source_ = (base_ / "lib" / "IMG_0001.JPG").string();
        writeAll(source_, sourceJpeg(6));
        events_.clear();
        Exporter::instance().configure(
            Exporter::Defaults{"jpeg", 92},
            [](const std::string &path) { return ChainEngine::decodeBgr8(path, 0); },
            [this](const std::string &path, bool ok, const std::string &error) {
                events_.push_back({path, ok, error});
            });
    }
    void TearDown() override
    {
        Exporter::instance().reset();
        fs::remove_all(base_);
    }

    ExportRequest request(const std::string &format = "jpeg")
    {
        ExportRequest r;
        r.file_path = source_;
        r.folder = (base_ / "out").string();
        r.format = format;
        return r;
    }

    struct Event
    {
        std::string path;
        bool ok;
        std::string error;
    };
    static int counter_;
    fs::path base_;
    std::string source_;
    std::vector<Event> events_;
};
int ExportTest::counter_ = 0;

TEST_F(ExportTest, HelpersNormaliseFormatsAndSuffixes)
{
    EXPECT_EQ(Exporter::normalizeFormat("JPG"), "jpeg");
    EXPECT_EQ(Exporter::normalizeFormat("tif"), "tiff");
    EXPECT_EQ(Exporter::normalizeFormat("webp"), "");
    EXPECT_EQ(Exporter::extensionFor("jpeg"), "jpg");
    EXPECT_EQ(Exporter::extensionFor("tiff"), "tif");
    EXPECT_EQ(Exporter::sanitizeSuffix(""), "edit");
    EXPECT_EQ(Exporter::sanitizeSuffix("  Moody Film. "), "Moody Film");
    EXPECT_EQ(Exporter::sanitizeSuffix("a/b\\c:d"), "a-b-c-d");
}

TEST_F(ExportTest, WritesANewFileAndNeverOverwrites)
{
    const fs::path out = base_ / "out";
    // Something already sits at the third name: it must survive untouched.
    writeAll((out / "IMG_0001-edit (3).jpg").string(), {'k', 'e', 'e', 'p'});

    RenderPlan plan = monochromePlan();
    ExportResult a = Exporter::instance().run(request(), plan);
    ASSERT_EQ(a.outcome, ExportResult::Outcome::Ok) << a.error;
    EXPECT_EQ(fs::path(a.written_path).filename(), "IMG_0001-edit.jpg");
    ExportResult b = Exporter::instance().run(request(), plan);
    ASSERT_EQ(b.outcome, ExportResult::Outcome::Ok) << b.error;
    EXPECT_EQ(fs::path(b.written_path).filename(), "IMG_0001-edit (2).jpg");
    ExportResult c = Exporter::instance().run(request(), plan);
    ASSERT_EQ(c.outcome, ExportResult::Outcome::Ok) << c.error;
    EXPECT_EQ(fs::path(c.written_path).filename(), "IMG_0001-edit (4).jpg");

    EXPECT_EQ(readAll((out / "IMG_0001-edit (3).jpg").string()), std::vector<uint8_t>({'k', 'e', 'e', 'p'}));
    // Exactly the exports and the bystander: no sidecar, no temp file.
    EXPECT_EQ(listing(out), std::set<std::string>({"IMG_0001-edit.jpg", "IMG_0001-edit (2).jpg",
                                                   "IMG_0001-edit (3).jpg", "IMG_0001-edit (4).jpg"}));
    // The source and its folder are untouched.
    EXPECT_EQ(listing(base_ / "lib"), std::set<std::string>({"IMG_0001.JPG"}));
    EXPECT_EQ(readAll(source_), sourceJpeg(6));

    // The picture is the chain applied: monochrome, so the red and blue
    // halves come out as two greys, not two colours.
    cv::Mat exported = cv::imread(a.written_path, cv::IMREAD_COLOR);
    ASSERT_FALSE(exported.empty());
    cv::Vec3b px = exported.at<cv::Vec3b>(exported.rows / 2, exported.cols / 4);
    EXPECT_LE(std::abs(int(px[0]) - int(px[2])), 6);
    ASSERT_EQ(events_.size(), 3u);
    EXPECT_TRUE(events_[0].ok);
}

TEST_F(ExportTest, ExifTravelsUprightWithoutThumbnailAndWithSoftwareVurio)
{
    ExportResult r = Exporter::instance().run(request(), RenderPlan::identity());
    ASSERT_EQ(r.outcome, ExportResult::Outcome::Ok) << r.error;
    EXPECT_TRUE(r.exif_copied);
    // Orientation 6 on a 640x480 source: the decode is upright, 480x640.
    EXPECT_EQ(r.width, 480);
    EXPECT_EQ(r.height, 640);
    cv::Mat exported = cv::imread(r.written_path, cv::IMREAD_COLOR);
    EXPECT_EQ(exported.cols, 480);
    EXPECT_EQ(exported.rows, 640);

    Parsed exif(readAll(r.written_path));
    EXPECT_EQ(exif.text(EXIF_TAG_MAKE), "Vurio Test Cameras");
    EXPECT_EQ(exif.text(EXIF_TAG_MODEL), "VT-1");
    EXPECT_EQ(exif.text(EXIF_TAG_SOFTWARE), "Vurio");
    EXPECT_FALSE(exif.has(EXIF_TAG_ORIENTATION));
    EXPECT_EQ(exif.ed->size, 0u) << "thumbnail must be dropped";
    EXPECT_EQ(exif.ed->ifd[EXIF_IFD_1]->count, 0u);
    EXPECT_EQ(exif.text(EXIF_TAG_EXPOSURE_TIME), "1/250 sec.");
    EXPECT_EQ(exif.text(EXIF_TAG_ISO_SPEED_RATINGS), "400");
    EXPECT_EQ(exif.text(EXIF_TAG_PIXEL_X_DIMENSION), "480");
    EXPECT_EQ(exif.text(EXIF_TAG_PIXEL_Y_DIMENSION), "640");
    EXPECT_TRUE(exif.has(static_cast<ExifTag>(EXIF_TAG_GPS_ALTITUDE)));
}

TEST_F(ExportTest, PngCarriesAnExifChunkAndTiffIsSixteenBitWithExifIfd)
{
    ExportResult png = Exporter::instance().run(request("png"), RenderPlan::identity());
    ASSERT_EQ(png.outcome, ExportResult::Outcome::Ok) << png.error;
    EXPECT_EQ(fs::path(png.written_path).extension(), ".png");
    std::vector<uint8_t> bytes = readAll(png.written_path);
    cv::Mat decoded = cv::imdecode(bytes, cv::IMREAD_COLOR);
    EXPECT_EQ(decoded.cols, 480);
    // The eXIf chunk parses back with libexif (header re-attached).
    std::string chunk_type(bytes.begin() + 8 + 12 + 13 + 4, bytes.begin() + 8 + 12 + 13 + 8);
    EXPECT_EQ(chunk_type, "eXIf");
    {
        uint32_t n = (uint32_t(bytes[33]) << 24) | (uint32_t(bytes[34]) << 16) | (uint32_t(bytes[35]) << 8) | bytes[36];
        std::vector<uint8_t> exif = {'E', 'x', 'i', 'f', 0, 0};
        exif.insert(exif.end(), bytes.begin() + 41, bytes.begin() + 41 + n);
        Parsed parsed(exif);
        EXPECT_EQ(parsed.text(EXIF_TAG_MAKE), "Vurio Test Cameras");
        EXPECT_EQ(parsed.text(EXIF_TAG_SOFTWARE), "Vurio");
        EXPECT_FALSE(parsed.has(EXIF_TAG_ORIENTATION));
    }

    ExportResult tiff = Exporter::instance().run(request("tiff"), RenderPlan::identity());
    ASSERT_EQ(tiff.outcome, ExportResult::Outcome::Ok) << tiff.error;
    EXPECT_EQ(fs::path(tiff.written_path).extension(), ".tif");
    cv::Mat t = cv::imread(tiff.written_path, cv::IMREAD_UNCHANGED);
    ASSERT_FALSE(t.empty());
    EXPECT_EQ(t.depth(), CV_16U);
    EXPECT_EQ(t.channels(), 3);
    EXPECT_EQ(t.cols, 480);
    EXPECT_EQ(t.rows, 640);
    TIFF *tif = TIFFOpen(tiff.written_path.c_str(), "r");
    ASSERT_NE(tif, nullptr);
    char *software = nullptr;
    char *make = nullptr;
    uint16_t orientation = 0;
    ASSERT_EQ(TIFFGetField(tif, TIFFTAG_SOFTWARE, &software), 1);
    EXPECT_STREQ(software, "Vurio");
    ASSERT_EQ(TIFFGetField(tif, TIFFTAG_MAKE, &make), 1);
    EXPECT_STREQ(make, "Vurio Test Cameras");
    TIFFGetFieldDefaulted(tif, TIFFTAG_ORIENTATION, &orientation);
    EXPECT_EQ(orientation, ORIENTATION_TOPLEFT);
    uint64_t exif_offset = 0;
    ASSERT_EQ(TIFFGetField(tif, TIFFTAG_EXIFIFD, &exif_offset), 1);
    ASSERT_EQ(TIFFReadEXIFDirectory(tif, exif_offset), 1);
    float exposure = 0; // libtiff stores EXIF rationals as float
    ASSERT_EQ(TIFFGetField(tif, EXIFTAG_EXPOSURETIME, &exposure), 1);
    EXPECT_NEAR(exposure, 1.0f / 250.0f, 1e-6f);
    uint16_t iso_count = 0;
    uint16_t *iso = nullptr;
    ASSERT_EQ(TIFFGetField(tif, EXIFTAG_ISOSPEEDRATINGS, &iso_count, &iso), 1);
    ASSERT_EQ(iso_count, 1);
    EXPECT_EQ(iso[0], 400);
    TIFFClose(tif);
}

TEST_F(ExportTest, LongSideLimitRecipeNameAndRefusals)
{
    ExportRequest r = request();
    r.max_side = 300;
    r.name_suffix = "Moody Film";
    r.quality = 80;
    ExportResult ok = Exporter::instance().run(r, RenderPlan::identity());
    ASSERT_EQ(ok.outcome, ExportResult::Outcome::Ok) << ok.error;
    EXPECT_EQ(fs::path(ok.written_path).filename(), "IMG_0001-Moody Film.jpg");
    EXPECT_EQ(std::max(ok.width, ok.height), 300);

    r = request("webp");
    EXPECT_EQ(Exporter::instance().run(r, RenderPlan::identity()).outcome, ExportResult::Outcome::Invalid);
    r = request();
    r.quality = 101;
    EXPECT_EQ(Exporter::instance().run(r, RenderPlan::identity()).outcome, ExportResult::Outcome::Invalid);
    r = request();
    r.folder = (base_ / "missing").string();
    EXPECT_EQ(Exporter::instance().run(r, RenderPlan::identity()).outcome, ExportResult::Outcome::Invalid);
    r = request();
    r.file_path = (base_ / "lib" / "gone.jpg").string();
    EXPECT_EQ(Exporter::instance().run(r, RenderPlan::identity()).outcome, ExportResult::Outcome::NotFound);
    if (::geteuid() != 0)
    {
        fs::create_directories(base_ / "locked");
        ::chmod((base_ / "locked").c_str(), 0500);
        r = request();
        r.folder = (base_ / "locked").string();
        EXPECT_EQ(Exporter::instance().run(r, RenderPlan::identity()).outcome, ExportResult::Outcome::Denied);
        ::chmod((base_ / "locked").c_str(), 0700);
    }
    // Every attempt, failed or not, told the notifier.
    EXPECT_GE(events_.size(), 5u);
    EXPECT_FALSE(events_.back().ok);
    EXPECT_FALSE(events_.back().error.empty());
    // Nothing from the failed attempts landed anywhere.
    EXPECT_EQ(listing(base_ / "out"), std::set<std::string>({"IMG_0001-Moody Film.jpg"}));
}

TEST_F(ExportTest, SourcesWithoutExifExportWithOnlyASoftwareNote)
{
    // A bare JPEG (no APP1) still exports; the warning says why nothing was copied.
    cv::Mat img(100, 120, CV_8UC3, cv::Scalar(1, 2, 3));
    std::vector<uchar> jpg;
    cv::imencode(".jpg", img, jpg);
    const std::string bare = (base_ / "lib" / "bare.jpg").string();
    writeAll(bare, std::vector<uint8_t>(jpg.begin(), jpg.end()));
    ExportRequest r = request();
    r.file_path = bare;
    ExportResult res = Exporter::instance().run(r, RenderPlan::identity());
    ASSERT_EQ(res.outcome, ExportResult::Outcome::Ok) << res.error;
    EXPECT_FALSE(res.exif_copied);
    EXPECT_FALSE(res.warning.empty());
    EXPECT_EQ(fs::path(res.written_path).filename(), "bare-edit.jpg");

    // A Fujifilm RAF: fixed header, embedded JPEG at the offset bytes 84..91 name.
    {
        std::vector<uint8_t> jpeg = sourceJpeg(8);
        std::vector<uint8_t> raf(160, 0);
        std::memcpy(raf.data(), "FUJIFILMCCD-RAW 0201FF129502", 28);
        const uint32_t offset = 160, length = static_cast<uint32_t>(jpeg.size());
        for (int i = 0; i < 4; ++i)
        {
            raf[84 + i] = static_cast<uint8_t>(offset >> (24 - 8 * i));
            raf[88 + i] = static_cast<uint8_t>(length >> (24 - 8 * i));
        }
        raf.insert(raf.end(), jpeg.begin(), jpeg.end());
        std::string raf_note;
        std::vector<uint8_t> from_raf = ExifTransplant::fromBytes(raf, 30, 40, 0, &raf_note);
        ASSERT_FALSE(from_raf.empty()) << raf_note;
        Parsed parsed_raf(from_raf);
        EXPECT_EQ(parsed_raf.text(EXIF_TAG_MODEL), "VT-1");
        EXPECT_FALSE(parsed_raf.has(EXIF_TAG_ORIENTATION));
        EXPECT_EQ(parsed_raf.text(EXIF_TAG_PIXEL_X_DIMENSION), "30");
    }

    // TIFF-structured sources are read through the Exif-header trick.
    std::vector<uint8_t> tiff_exif = cameraExif(1);
    std::vector<uint8_t> as_tiff(tiff_exif.begin() + 6, tiff_exif.end());
    std::string note;
    std::vector<uint8_t> blob = ExifTransplant::fromBytes(as_tiff, 10, 20, 0, &note);
    ASSERT_FALSE(blob.empty()) << note;
    Parsed parsed(blob);
    EXPECT_EQ(parsed.text(EXIF_TAG_MAKE), "Vurio Test Cameras");
    EXPECT_FALSE(parsed.has(EXIF_TAG_ORIENTATION));
    EXPECT_FALSE(ExifTransplant::hasOrientation(blob));
    EXPECT_TRUE(ExifTransplant::hasOrientation(cameraExif(3)));
}

TEST_F(ExportTest, AnExportInsideARegisteredLocationIsJustANewFileForTheScan)
{
    // The library folder itself is the destination.
    ExportRequest r = request();
    r.folder = (base_ / "lib").string();
    ExportResult res = Exporter::instance().run(r, monochromePlan());
    ASSERT_EQ(res.outcome, ExportResult::Outcome::Ok) << res.error;

    std::set<std::string> seen;
    MediaDedup::Files::scan(base_ / "lib", [&](const MediaDedup::Files::FileRecord &rec) {
        if (!rec.hasError())
            seen.insert(fs::path(rec.fullPath).filename().string());
    });
    // The scan sees the original and the export — and nothing else: no
    // sidecar, no marker, no temp file.
    EXPECT_EQ(seen, std::set<std::string>({"IMG_0001.JPG", "IMG_0001-edit.jpg"}));
    EXPECT_EQ(listing(base_ / "lib"), seen);
}

// CHAIN-7h: the AI finishing steps. Refusals need no model; the runs are
// gated on the optional files being installed (ensure_models.sh).
TEST_F(ExportTest, AiFinishingRefusesBadRequestsAndRunsWhenTheModelsArePresent)
{
    ExportRequest r = request();
    r.denoise = 1.5;
    EXPECT_EQ(Exporter::instance().run(r, RenderPlan::identity()).outcome, ExportResult::Outcome::Invalid);
    r = request();
    r.upscale = 3;
    EXPECT_EQ(Exporter::instance().run(r, RenderPlan::identity()).outcome, ExportResult::Outcome::Invalid);

    ExportModels::Models models;
    models.denoise_model = "models/nafnet-sidd-w32.onnx";
    models.upscale_model = "models/realesrgan-x2.onnx";
    ExportModels::configure(models);
    if (!ExportModels::denoiseAvailable() || !ExportModels::upscaleAvailable())
    {
        // Without the files the request is refused with the path in the message.
        r = request();
        r.denoise = 0.5;
        ExportResult missing = Exporter::instance().run(r, RenderPlan::identity());
        if (!ExportModels::denoiseAvailable())
        {
            EXPECT_EQ(missing.outcome, ExportResult::Outcome::Invalid);
            EXPECT_NE(missing.error.find("nafnet"), std::string::npos);
        }
        GTEST_SKIP() << "export models not installed";
    }

    // A noisy flat source: denoise at full strength cuts the noise, the
    // half-strength run lands in between; ×2 doubles the dimensions.
    cv::Mat noisy(240, 320, CV_8UC3, cv::Scalar(120, 110, 100));
    cv::Mat noise(noisy.size(), CV_8SC3);
    cv::RNG rng(7);
    rng.fill(noise, cv::RNG::NORMAL, 0, 12);
    cv::add(noisy, noise, noisy, cv::noArray(), CV_8UC3);
    std::vector<uint8_t> bytes;
    cv::imencode(".png", noisy, bytes);
    const std::string noisy_path = (base_ / "lib" / "noisy.png").string();
    writeAll(noisy_path, bytes);
    auto stddev = [](const std::string &path) {
        cv::Mat img = cv::imread(path);
        cv::Scalar mean, sd;
        cv::meanStdDev(img, mean, sd);
        return sd[1];
    };
    const double before = stddev(noisy_path);
    ASSERT_GT(before, 6.0);

    r = request("png");
    r.file_path = noisy_path;
    r.denoise = 1.0;
    ExportResult full = Exporter::instance().run(r, RenderPlan::identity());
    ASSERT_EQ(full.outcome, ExportResult::Outcome::Ok) << full.error;
    const double after_full = stddev(full.written_path);
    EXPECT_LT(after_full, before * 0.5) << "the noise is mostly gone";
    r.denoise = 0.5;
    ExportResult half = Exporter::instance().run(r, RenderPlan::identity());
    ASSERT_EQ(half.outcome, ExportResult::Outcome::Ok) << half.error;
    const double after_half = stddev(half.written_path);
    EXPECT_GT(after_half, after_full);
    EXPECT_LT(after_half, before);

    r = request();
    r.upscale = 2;
    r.max_side = 200;
    ExportResult up = Exporter::instance().run(r, RenderPlan::identity());
    ASSERT_EQ(up.outcome, ExportResult::Outcome::Ok) << up.error;
    EXPECT_EQ(std::max(up.width, up.height), 400) << "limit first, then ×2";
    cv::Mat written = cv::imread(up.written_path);
    EXPECT_EQ(written.cols, up.width);
    ExportModels::unload();
}
