#include "search/location_geocoder.hpp"

#include <Poco/JSON/Array.h>
#include <Poco/JSON/Object.h>
#include <Poco/JSON/Parser.h>
#include <Poco/Logger.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <limits>

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

        // Recursively fold every [lon, lat] leaf of a GeoJSON coordinates
        // array into the running bounds. Handles Point through MultiPolygon
        // without caring which geometry type it is.
        void foldCoordinates(const Poco::Dynamic::Var &node, LocationGeocoder::Bounds &b, bool &any)
        {
            Poco::JSON::Array::Ptr arr = node.extract<Poco::JSON::Array::Ptr>();
            if (arr.isNull() || arr->size() == 0)
                return;

            // A coordinate leaf is an array whose first element is numeric.
            if (arr->get(0).isNumeric())
            {
                if (arr->size() < 2)
                    return;
                double lon = arr->get(0).convert<double>();
                double lat = arr->get(1).convert<double>();
                if (!any)
                {
                    b.lon_min = b.lon_max = lon;
                    b.lat_min = b.lat_max = lat;
                    any = true;
                }
                else
                {
                    b.lon_min = std::min(b.lon_min, lon);
                    b.lon_max = std::max(b.lon_max, lon);
                    b.lat_min = std::min(b.lat_min, lat);
                    b.lat_max = std::max(b.lat_max, lat);
                }
                return;
            }

            for (std::size_t i = 0; i < arr->size(); ++i)
            {
                try
                {
                    foldCoordinates(arr->get(i), b, any);
                }
                catch (...) {}
            }
        }

        double area(const LocationGeocoder::Bounds &b)
        {
            return (b.lat_max - b.lat_min) * (b.lon_max - b.lon_min);
        }
    }

    bool LocationGeocoder::loadFromFile(const std::string &geojson_path)
    {
        Poco::Logger &logger = Poco::Logger::get("LocationGeocoder");
        by_name_.clear();

        std::ifstream in(geojson_path);
        if (!in)
        {
            logger.warning("cannot open gazetteer: %s", geojson_path);
            return false;
        }

        Poco::JSON::Object::Ptr root;
        try
        {
            Poco::JSON::Parser parser;
            root = parser.parse(in).extract<Poco::JSON::Object::Ptr>();
        }
        catch (const std::exception &e)
        {
            logger.warning("malformed gazetteer %s: %s", geojson_path, std::string(e.what()));
            return false;
        }
        if (root.isNull())
            return false;

        Poco::JSON::Array::Ptr features = root->getArray("features");
        if (features.isNull())
        {
            logger.warning("gazetteer has no features array: %s", geojson_path);
            return false;
        }

        static const char *kNameKeys[] = {"name", "NAME", "Name", "name_en", "NAME_EN",
                                          "admin", "ADMIN", "unit_name", "UNIT_NAME"};

        for (std::size_t i = 0; i < features->size(); ++i)
        {
            Poco::JSON::Object::Ptr feature = features->getObject(static_cast<unsigned>(i));
            if (feature.isNull())
                continue;

            Bounds bounds;
            bool have_bounds = false;

            // Prefer an explicit bbox: [west, south, east, north].
            if (feature->has("bbox") && !feature->isNull("bbox"))
            {
                Poco::JSON::Array::Ptr bbox = feature->getArray("bbox");
                if (!bbox.isNull() && bbox->size() >= 4)
                {
                    try
                    {
                        bounds.lon_min = bbox->get(0).convert<double>();
                        bounds.lat_min = bbox->get(1).convert<double>();
                        bounds.lon_max = bbox->get(2).convert<double>();
                        bounds.lat_max = bbox->get(3).convert<double>();
                        have_bounds = true;
                    }
                    catch (...) {}
                }
            }

            // Otherwise compute one from the geometry.
            if (!have_bounds && feature->has("geometry") && !feature->isNull("geometry"))
            {
                Poco::JSON::Object::Ptr geometry = feature->getObject("geometry");
                if (!geometry.isNull() && geometry->has("coordinates"))
                {
                    try
                    {
                        foldCoordinates(geometry->get("coordinates"), bounds, have_bounds);
                    }
                    catch (...) {}
                }
            }
            if (!have_bounds)
                continue;

            Poco::JSON::Object::Ptr props = feature->getObject("properties");
            if (props.isNull())
                continue;
            std::string kind;
            if (props->has("kind") && !props->isNull("kind"))
            {
                try
                {
                    kind = props->get("kind").convert<std::string>();
                }
                catch (...) {}
            }
            for (const char *key : kNameKeys)
            {
                if (!props->has(key) || props->isNull(key))
                    continue;
                std::string name;
                try
                {
                    name = props->get(key).convert<std::string>();
                }
                catch (...) { continue; }
                std::string lowered = toLower(name);
                if (!lowered.empty())
                    by_name_.emplace(lowered, Entry{name, kind, bounds}); // first writer wins
            }
        }

        logger.information("loaded %z location(s) from %s", by_name_.size(), geojson_path);
        return true;
    }

    std::optional<LocationGeocoder::Bounds> LocationGeocoder::lookup(const std::string &location_name) const
    {
        std::string query = toLower(location_name);
        // Trim surrounding whitespace.
        auto b = query.find_first_not_of(" \t\r\n");
        if (b == std::string::npos)
            return std::nullopt;
        auto e = query.find_last_not_of(" \t\r\n");
        query = query.substr(b, e - b + 1);
        if (query.empty())
            return std::nullopt;

        auto exact = by_name_.find(query);
        if (exact != by_name_.end())
            return exact->second.bounds;

        // Substring pass: stored name contains the query. Too-short queries
        // would match half the gazetteer ("in" is in "China"), so gate it.
        if (query.size() < 3)
            return std::nullopt;

        const Bounds *best = nullptr;
        double best_area = std::numeric_limits<double>::max();
        for (const auto &[name, entry] : by_name_)
        {
            if (name.find(query) == std::string::npos)
                continue;
            double a = area(entry.bounds);
            if (a < best_area)
            {
                best_area = a;
                best = &entry.bounds;
            }
        }
        if (best)
            return *best;
        return std::nullopt;
    }

    std::vector<LocationGeocoder::Entry>
    LocationGeocoder::containing(double lat, double lon, const std::string &kind) const
    {
        std::vector<Entry> out;
        for (const auto &[name, entry] : by_name_)
        {
            if (!kind.empty() && entry.kind != kind)
                continue;
            const Bounds &b = entry.bounds;
            if (lat >= b.lat_min && lat <= b.lat_max && lon >= b.lon_min && lon <= b.lon_max)
                out.push_back(entry);
        }
        return out;
    }
}
