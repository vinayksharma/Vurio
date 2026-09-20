#include "search/exif_keyword_config.hpp"

#include <Poco/JSON/Array.h>
#include <Poco/JSON/Object.h>
#include <Poco/JSON/Parser.h>
#include <Poco/Logger.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

namespace MediaDedup
{
    namespace
    {
        std::string toLower(const std::string &s)
        {
            std::string out(s);
            std::transform(out.begin(), out.end(), out.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return out;
        }

        // If the JSON object has `key` as an array of strings, replace
        // `target` with its (lowercased, trimmed-of-empties) contents.
        void overlayList(const Poco::JSON::Object::Ptr &obj, const char *key,
                         std::vector<std::string> &target)
        {
            if (obj.isNull() || !obj->has(key))
                return;
            Poco::JSON::Array::Ptr arr = obj->getArray(key);
            if (arr.isNull())
                return;
            std::vector<std::string> values;
            for (std::size_t i = 0; i < arr->size(); ++i)
            {
                std::string v = toLower(arr->get(i).toString());
                if (!v.empty())
                    values.push_back(v);
            }
            target = std::move(values);
        }
    }

    ExifKeywordConfig ExifKeywordConfig::defaults()
    {
        ExifKeywordConfig c;

        c.camera_brands = {"canon", "nikon", "sony", "fuji", "fujifilm",
                           "panasonic", "lumix", "olympus", "leica", "pentax",
                           "hasselblad", "iphone", "apple", "samsung", "google",
                           "pixel", "dji", "gopro", "ricoh", "sigma", "kodak"};
        c.camera_context_verbs = {"taken", "shot", "captured", "photographed"};
        c.camera_context_prepositions = {"with", "on", "using", "by"};
        c.camera_articles = {"a", "an", "my"};
        c.camera_suffixes = {"camera"};

        c.greater_words = {"above", "over", "greater than", "more than",
                           "higher than", "bigger than", "larger than",
                           "greater", "higher", "bigger", "larger", ">"};
        c.greater_equal_words = {"at least", "no less than", "not less than",
                                 "minimum", "min", ">=", "=>"};
        c.less_words = {"below", "under", "less than", "lower than",
                        "smaller than", "fewer than", "less", "lower",
                        "smaller", "<"};
        c.less_equal_words = {"at most", "no more than", "not more than",
                              "maximum", "max", "<=", "=<"};
        c.aperture_wider_words = {"wider than", "wider", "faster", "brighter",
                                  "more open"};
        c.aperture_narrower_words = {"narrower than", "narrower", "smaller",
                                     "stopped down"};
        c.shutter_faster_words = {"faster than", "faster", "quicker",
                                  "shorter than", "shorter"};
        c.shutter_slower_words = {"slower than", "slower", "longer than",
                                  "longer"};
        c.focal_longer_words = {"above", "over", "longer than", "greater than",
                                "more than", ">"};
        c.focal_shorter_words = {"below", "under", "shorter than", "less than", "<"};

        c.flash_on_phrases = {"with flash", "flash on", "flash fired", "using flash"};
        c.flash_off_phrases = {"without flash", "no flash", "flash off"};

        c.months = {"january", "february", "march", "april", "may", "june",
                    "july", "august", "september", "october", "november", "december"};
        c.ambiguous_months = {"may"};
        c.month_prepositions = {"in", "from", "during"};

        c.location_prepositions = {"in", "at", "near", "around", "from"};

        // Alphabetical by name — file_types is kept sorted (JSON object
        // iteration order is not insertion order, so sorted is the only
        // stable contract).
        c.file_types = {
            {"heic", {"heic", "heif"}},
            {"jpeg", {"jpg", "jpeg"}},
            {"png", {"png"}},
            {"raw", {"cr2", "cr3", "nef", "nrw", "arw", "raf", "orf", "rw2", "dng", "pef", "srw", "x3f"}},
            {"tiff", {"tif", "tiff"}},
            {"video", {"mp4", "mov", "m4v", "avi", "mkv", "webm"}},
        };

        // PLACE-4: alphabetical by name (same contract as file_types).
        c.settings = {
            {"beach", {"T.BCH", "H.BCH"}, false},
            {"canyon", {"T.CNYN", "T.GRGE", "T.VAL"}, false},
            {"forest", {"L.RESF", "L.RESW", "L.RESN"}, false},
            {"glacier", {"H.GLCR"}, false},
            {"island", {"T.ISL", "T.ISLS", "T.ISLET"}, false},
            {"lake", {"H.LK", "H.LKS", "H.RSV", "H.PND", "H.LGN", "H.LKN", "H.LKI"}, false},
            {"mountain", {"T.MT", "T.MTS", "T.PK", "T.HLL", "T.HLLS", "T.RDGE", "T.PASS", "T.VLC"}, false},
            {"park", {"L.PRK", "L.RES", "L.RESN", "L.RESW", "L.RESF", "L.RESH", "L.RESA"}, false},
            {"spring", {"H.SPNG", "H.HTSP", "H.GYSR"}, false},
            {"trail", {"R.TRL"}, false},
            {"waterfall", {"H.FLLS", "H.OVF"}, false},
            {"waterfront", {"H.BAY", "H.COVE", "H.HBR", "H.INLT", "H.LGN", "H.SD", "H.STRT", "H.CHN",
                            "H.SEA", "H.OCN", "H.GULF", "H.FJD", "H.ESTY", "H.ANCH", "H.LK", "H.LKS",
                            "H.RSV", "H.PND", "T.BCH", "H.BCH", "S.PIER", "S.MAR", "L.PRT", "H.STR"}, true},
        };

        c.imperative_words = {"find", "show", "me", "all"};
        c.filler_words = {"find", "show", "me", "all", "the", "a", "an", "my", "of",
                          "photo", "photos", "picture", "pictures", "pic", "pics",
                          "image", "images", "shot", "shots", "taken", "with", "and"};
        return c;
    }

    bool ExifKeywordConfig::loadFromFile(const std::string &json_path)
    {
        Poco::Logger &logger = Poco::Logger::get("ExifKeywordConfig");

        std::ifstream in(json_path);
        if (!in)
        {
            logger.warning("Keyword file not found: %s", json_path);
            return false;
        }
        std::stringstream buffer;
        buffer << in.rdbuf();

        Poco::JSON::Object::Ptr root;
        try
        {
            Poco::JSON::Parser parser;
            root = parser.parse(buffer.str()).extract<Poco::JSON::Object::Ptr>();
        }
        catch (const std::exception &e)
        {
            logger.error("Failed to parse keyword file %s: %s", json_path, std::string(e.what()));
            return false;
        }
        if (root.isNull())
        {
            logger.error("Keyword file %s is not a JSON object", json_path);
            return false;
        }

        auto section = [&](const char *name) -> Poco::JSON::Object::Ptr {
            return root->has(name) ? root->getObject(name) : Poco::JSON::Object::Ptr();
        };

        Poco::JSON::Object::Ptr camera = section("camera");
        overlayList(camera, "brands", camera_brands);
        overlayList(camera, "context_verbs", camera_context_verbs);
        overlayList(camera, "context_prepositions", camera_context_prepositions);
        overlayList(camera, "articles", camera_articles);
        overlayList(camera, "suffixes", camera_suffixes);

        Poco::JSON::Object::Ptr cmp = section("comparisons");
        overlayList(cmp, "greater", greater_words);
        overlayList(cmp, "greater_or_equal", greater_equal_words);
        overlayList(cmp, "less", less_words);
        overlayList(cmp, "less_or_equal", less_equal_words);
        overlayList(cmp, "aperture_wider", aperture_wider_words);
        overlayList(cmp, "aperture_narrower", aperture_narrower_words);
        overlayList(cmp, "shutter_faster", shutter_faster_words);
        overlayList(cmp, "shutter_slower", shutter_slower_words);
        overlayList(cmp, "focal_longer", focal_longer_words);
        overlayList(cmp, "focal_shorter", focal_shorter_words);

        Poco::JSON::Object::Ptr flash = section("flash");
        overlayList(flash, "on_phrases", flash_on_phrases);
        overlayList(flash, "off_phrases", flash_off_phrases);

        Poco::JSON::Object::Ptr dates = section("dates");
        // A month list that isn't exactly 12 entries would silently corrupt
        // month-number mapping; refuse it but keep loading everything else.
        if (!dates.isNull() && dates->has("months"))
        {
            std::vector<std::string> months_in = months;
            overlayList(dates, "months", months_in);
            if (months_in.size() == 12)
                months = std::move(months_in);
            else
                logger.error("Keyword file %s: 'dates.months' must have exactly 12 entries "
                             "(got %z); keeping defaults",
                             json_path, months_in.size());
        }
        overlayList(dates, "ambiguous_months", ambiguous_months);
        overlayList(dates, "prepositions", month_prepositions);

        Poco::JSON::Object::Ptr location = section("location");
        overlayList(location, "prepositions", location_prepositions);

        // file_types: an object of {type name -> [extensions]}. Present
        // replaces the whole map (same wholesale semantics as the lists).
        if (root->has("file_types"))
        {
            Poco::JSON::Object::Ptr ft = root->getObject("file_types");
            if (!ft.isNull())
            {
                std::vector<std::pair<std::string, std::vector<std::string>>> types;
                for (const auto &name : *ft)
                {
                    std::vector<std::string> exts;
                    Poco::JSON::Array::Ptr arr = ft->getArray(name.first);
                    if (!arr.isNull())
                        for (std::size_t i = 0; i < arr->size(); ++i)
                        {
                            std::string v = toLower(arr->get(i).toString());
                            if (!v.empty())
                                exts.push_back(v);
                        }
                    if (!exts.empty())
                        types.emplace_back(toLower(name.first), std::move(exts));
                }
                std::sort(types.begin(), types.end(),
                          [](const auto &a, const auto &b) { return a.first < b.first; });
                file_types = std::move(types);
            }
        }

        // PLACE-4 settings: {category -> {"codes": [...], "shoreline": bool}}.
        if (root->has("settings"))
        {
            Poco::JSON::Object::Ptr st = root->getObject("settings");
            if (!st.isNull())
            {
                std::vector<SettingCategory> cats;
                for (const auto &name : *st)
                {
                    Poco::JSON::Object::Ptr entry = st->getObject(name.first);
                    if (entry.isNull())
                        continue;
                    SettingCategory cat;
                    cat.name = toLower(name.first);
                    Poco::JSON::Array::Ptr codes = entry->getArray("codes");
                    if (!codes.isNull())
                        for (std::size_t i = 0; i < codes->size(); ++i)
                        {
                            std::string v = codes->get(i).toString();
                            if (!v.empty())
                                cat.codes.push_back(v);
                        }
                    cat.shoreline = entry->optValue<bool>("shoreline", false);
                    if (!cat.name.empty() && !cat.codes.empty())
                        cats.push_back(std::move(cat));
                }
                std::sort(cats.begin(), cats.end(),
                          [](const auto &a, const auto &b) { return a.name < b.name; });
                settings = std::move(cats);
            }
        }

        Poco::JSON::Object::Ptr text = section("text");
        overlayList(text, "imperative_words", imperative_words);
        overlayList(text, "filler_words", filler_words);

        logger.information("Loaded EXIF search keywords from %s (%z camera brands)",
                           json_path, camera_brands.size());
        return true;
    }

    const ExifKeywordConfig::SettingCategory *
    ExifKeywordConfig::findSetting(const std::string &name) const
    {
        auto lookup = [&](const std::string &n) -> const SettingCategory * {
            for (const auto &c : settings)
                if (c.name == n)
                    return &c;
            return nullptr;
        };
        if (const auto *c = lookup(name))
            return c;
        if (name.size() > 2 && name.back() == 's')
            return lookup(name.substr(0, name.size() - 1));
        return nullptr;
    }
}
