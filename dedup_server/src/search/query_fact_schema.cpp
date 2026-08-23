#include "search/query_fact_schema.hpp"

#include "search/exif_keyword_config.hpp"

#include <set>
#include <sstream>

namespace MediaDedup
{
    QueryFactSchema QueryFactSchema::fromKeywords(const ExifKeywordConfig &kw)
    {
        QueryFactSchema s;

        std::vector<std::string> type_names;
        for (const auto &[name, exts] : kw.file_types)
        {
            (void)exts;
            type_names.push_back(name);
        }

        s.dims_ = {
            {"camera",
             "camera brand or model words from the query",
             {"in"},
             kw.camera_brands},
            {"iso",
             "ISO sensitivity; values are plain numbers",
             {"eq", "gt", "gte", "lt", "lte", "range"},
             {}},
            {"aperture",
             "aperture f-number; values are numbers (2.8 for f/2.8); "
             "wider aperture = smaller number",
             {"eq", "gt", "lt", "range"},
             {}},
            {"shutter",
             "shutter speed exactly as written (1/500, 2); faster = shorter "
             "exposure",
             {"eq", "faster", "slower", "range"},
             {}},
            {"focal",
             "focal length in mm; values are numbers (50 for 50mm)",
             {"eq", "gt", "lt", "range"},
             {}},
            {"flash",
             "whether the flash fired; values stay empty",
             {"on", "off"},
             {}},
            {"date",
             "capture date; values exactly as written (2024, august, "
             "august 2024); range for between/from-to",
             {"eq", "range"},
             {}},
            {"location",
             "place name, ONLY when anchored by a preposition (in/at/near/"
             "around/from); bare place names stay in clip_queries",
             {"eq"},
             {}},
            {"file_type",
             "file format by name",
             {"in"},
             type_names},
        };
        return s;
    }

    const FactDimension *QueryFactSchema::find(const std::string &name) const
    {
        for (const auto &d : dims_)
            if (d.name == name)
                return &d;
        return nullptr;
    }

    std::string QueryFactSchema::promptSchemaSection() const
    {
        std::ostringstream out;
        for (const auto &d : dims_)
        {
            out << "- dim \"" << d.name << "\" (ops: ";
            for (std::size_t i = 0; i < d.ops.size(); ++i)
                out << (i ? ", " : "") << d.ops[i];
            out << "): " << d.description;
            if (!d.hints.empty())
            {
                out << ". Known values: ";
                for (std::size_t i = 0; i < d.hints.size(); ++i)
                    out << (i ? ", " : "") << d.hints[i];
            }
            out << "\n";
        }
        return out.str();
    }

    std::string QueryFactSchema::gbnf() const
    {
        // Alternation of quoted literals: "\"camera\"" | "\"iso\"" | ...
        auto literalAlt = [](const std::vector<std::string> &words) {
            std::string out;
            for (std::size_t i = 0; i < words.size(); ++i)
            {
                if (i)
                    out += " | ";
                out += "\"\\\"" + words[i] + "\\\"\"";
            }
            return out;
        };

        std::vector<std::string> dim_names;
        std::set<std::string> op_set;
        for (const auto &d : dims_)
        {
            dim_names.push_back(d.name);
            op_set.insert(d.ops.begin(), d.ops.end());
        }
        std::vector<std::string> ops(op_set.begin(), op_set.end());

        // Every repetition is BOUNDED ({m,n}) so a grammar-constrained
        // decode always terminates: after the bound, the only valid
        // continuation is the closing literal and the sampler masks
        // everything else. Bounds double as sane production limits
        // (8 facts, 8 values each, 4 clip segments, 120-char strings).
        std::ostringstream g;
        g << "root ::= \"{\\\"facts\\\": [\" (fact (\", \" fact){0,7})? "
             "\"], \\\"clip_queries\\\": [\" (string (\", \" string){0,3})? \"]}\"\n";
        g << "fact ::= \"{\\\"dim\\\": \" dim \", \\\"op\\\": \" op "
             "\", \\\"values\\\": [\" (string (\", \" string){0,7})? \"], "
             "\\\"intent\\\": \" intent \", \\\"span\\\": \" string \"}\"\n";
        g << "dim ::= " << literalAlt(dim_names) << "\n";
        g << "op ::= " << literalAlt(ops) << "\n";
        g << "intent ::= " << literalAlt({"filter", "explore"}) << "\n";
        // Strings: escapes allowed, no raw control characters.
        g << "string ::= \"\\\"\" char{0,120} \"\\\"\"\n";
        g << "char ::= [^\"\\\\\\x00-\\x1f] | \"\\\\\" [\"\\\\bfnrt]\n";
        return g.str();
    }
}
