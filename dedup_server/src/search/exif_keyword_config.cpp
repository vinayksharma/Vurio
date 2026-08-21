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

        c.greater_words = {"above", "over", "greater than", "more than"};
        c.greater_equal_words = {"at least"};
        c.less_words = {"below", "under", "less than"};
        c.less_equal_words = {"at most"};
        c.aperture_wider_words = {"wider", "faster", "brighter"};
        c.aperture_narrower_words = {"narrower", "smaller"};
        c.shutter_faster_words = {"faster", "quicker"};
        c.shutter_slower_words = {"slower", "longer"};
        c.focal_longer_words = {"above", "over", "longer than"};
        c.focal_shorter_words = {"below", "under", "shorter than"};

        c.flash_on_phrases = {"with flash", "flash on", "flash fired", "using flash"};
        c.flash_off_phrases = {"without flash", "no flash", "flash off"};

        c.months = {"january", "february", "march", "april", "may", "june",
                    "july", "august", "september", "october", "november", "december"};
        c.ambiguous_months = {"may"};
        c.month_prepositions = {"in", "from", "during"};

        c.location_prepositions = {"in", "at", "near", "around", "from"};

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

        Poco::JSON::Object::Ptr text = section("text");
        overlayList(text, "imperative_words", imperative_words);
        overlayList(text, "filler_words", filler_words);

        logger.information("Loaded EXIF search keywords from %s (%z camera brands)",
                           json_path, camera_brands.size());
        return true;
    }
}
