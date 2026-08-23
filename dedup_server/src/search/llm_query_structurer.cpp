#include "search/llm_query_structurer.hpp"

#include "search/llm_runtime.hpp"

#include <Poco/JSON/Array.h>
#include <Poco/JSON/Object.h>
#include <Poco/JSON/Parser.h>
#include <Poco/Logger.h>

#include <sstream>

namespace MediaDedup
{
    namespace
    {
        // Few-shot examples — the worked examples from the EXIF-7/QP-1
        // specs, kept byte-stable (the system prompt is KV-prefix-cached).
        // Each output line must be exactly what the GBNF grammar produces.
        const char *kExamples = R"EX(Examples:

Query: sunset over the ocean
Output: {"facts": [], "clip_queries": ["sunset over the ocean"]}

Query: f2.5
Output: {"facts": [{"dim": "aperture", "op": "eq", "values": ["2.5"], "intent": "explore", "span": "f2.5"}], "clip_queries": ["f2.5"]}

Query: raw, sunset, fuji
Output: {"facts": [{"dim": "file_type", "op": "in", "values": ["raw"], "intent": "explore", "span": "raw"}, {"dim": "camera", "op": "in", "values": ["fuji"], "intent": "explore", "span": "fuji"}], "clip_queries": ["sunset", "fuji"]}

Query: sunset taken with a canon
Output: {"facts": [{"dim": "camera", "op": "in", "values": ["canon"], "intent": "filter", "span": "taken with a canon"}], "clip_queries": ["sunset"]}

Query: sunset 2024
Output: {"facts": [{"dim": "date", "op": "eq", "values": ["2024"], "intent": "filter", "span": "2024"}], "clip_queries": ["sunset"]}

Query: photos with flash in yellowstone
Output: {"facts": [{"dim": "flash", "op": "on", "values": [], "intent": "filter", "span": "with flash"}, {"dim": "location", "op": "eq", "values": ["yellowstone"], "intent": "filter", "span": "in yellowstone"}], "clip_queries": []}

Query: f2.5 to f12 but taken with fuji or canon between 2005 and 2015
Output: {"facts": [{"dim": "aperture", "op": "range", "values": ["2.5", "12"], "intent": "filter", "span": "f2.5 to f12"}, {"dim": "camera", "op": "in", "values": ["fuji", "canon"], "intent": "filter", "span": "taken with fuji or canon"}, {"dim": "date", "op": "range", "values": ["2005", "2015"], "intent": "filter", "span": "between 2005 and 2015"}], "clip_queries": []}

Query: Flowers, taken at f2.5 to f12, with fuji or canon, between 2005 and 2015
Output: {"facts": [{"dim": "aperture", "op": "range", "values": ["2.5", "12"], "intent": "filter", "span": "taken at f2.5 to f12"}, {"dim": "camera", "op": "in", "values": ["fuji", "canon"], "intent": "filter", "span": "with fuji or canon"}, {"dim": "date", "op": "range", "values": ["2005", "2015"], "intent": "filter", "span": "between 2005 and 2015"}], "clip_queries": ["flowers"]}

Query: shots faster than 1/500 iso above 800
Output: {"facts": [{"dim": "shutter", "op": "faster", "values": ["1/500"], "intent": "filter", "span": "faster than 1/500"}, {"dim": "iso", "op": "gt", "values": ["800"], "intent": "filter", "span": "iso above 800"}], "clip_queries": []}
)EX";
    }

    LlmQueryStructurer::LlmQueryStructurer(QueryFactSchema schema)
        : schema_(std::move(schema))
    {
        grammar_ = schema_.gbnf();

        std::ostringstream p;
        p << "You convert photo-library search queries into structured facts "
             "plus semantic text. Output ONLY the JSON object, nothing else.\n"
          << "\n"
          << "Fact dimensions:\n"
          << schema_.promptSchemaSection()
          << "\n"
          << "Rules:\n"
          << "- intent \"filter\": the query grammatically binds the fact — "
             "connecting words (taken with, at, using), comparatives (above, "
             "wider than, faster than), ranges (between X and Y, X to Y), and "
             "ALL dates. Filters restrict results.\n"
          << "- intent \"explore\": the fact is a lone token in a loose list "
             "(bare brand name, bare f2.5, bare file type). Explore facts "
             "widen results.\n"
          << "- span: quote the EXACT substring of the query each fact came "
             "from. Never invent facts that are not in the text.\n"
          << "- values: copy values as written, without units or f-prefixes "
             "(2.8 for f/2.8, 500 for iso 500, 50 for 50mm); ranges are "
             "[low, high].\n"
          << "- clip_queries: the semantic segments left over (scenes, "
             "subjects, moods), with command words (find, show me) dropped. "
             "A query that is only facts and filler gets [] — but a lone "
             "explore fact also repeats its text as a clip query.\n"
          << "- Unknown people, pets, or things stay in clip_queries; there "
             "is no fact dimension for them.\n"
          << "\n"
          << kExamples;
        system_prompt_ = p.str();
    }

    LlmQueryStructurer::RawOutput
    LlmQueryStructurer::structure(const std::string &query,
                                  const CompleteFn &complete) const
    {
        RawOutput out;
        Completion c = complete(system_prompt_, "Query: " + query, grammar_);
        if (!c.ok)
        {
            out.error = c.error.empty() ? "completion_failed" : c.error;
            return out;
        }

        try
        {
            Poco::JSON::Parser parser;
            auto root = parser.parse(c.text).extract<Poco::JSON::Object::Ptr>();
            if (root.isNull())
            {
                out.error = "not_a_json_object";
                return out;
            }

            Poco::JSON::Array::Ptr facts = root->getArray("facts");
            if (!facts.isNull())
            {
                for (std::size_t i = 0; i < facts->size(); ++i)
                {
                    Poco::JSON::Object::Ptr f = facts->getObject(i);
                    if (f.isNull())
                        continue;
                    RawFact fact;
                    fact.dim = f->optValue<std::string>("dim", "");
                    fact.op = f->optValue<std::string>("op", "");
                    fact.intent = f->optValue<std::string>("intent", "");
                    fact.span = f->optValue<std::string>("span", "");
                    Poco::JSON::Array::Ptr vals = f->getArray("values");
                    if (!vals.isNull())
                        for (std::size_t j = 0; j < vals->size(); ++j)
                            fact.values.push_back(vals->get(j).toString());
                    out.facts.push_back(std::move(fact));
                }
            }

            Poco::JSON::Array::Ptr clip = root->getArray("clip_queries");
            if (!clip.isNull())
                for (std::size_t i = 0; i < clip->size(); ++i)
                {
                    std::string q = clip->get(i).toString();
                    if (!q.empty())
                        out.clip_queries.push_back(std::move(q));
                }

            out.ok = true;
        }
        catch (const std::exception &e)
        {
            // The grammar makes this near-impossible; a truncated decode
            // (max_output_tokens hit mid-object) is the realistic path here.
            Poco::Logger::get("LlmQueryStructurer")
                .warning("Unparseable structurer output (%s): %s",
                         std::string(e.what()), c.text);
            out.error = "unparseable_output";
        }
        return out;
    }

    LlmQueryStructurer::RawOutput
    LlmQueryStructurer::structure(const std::string &query, LlmRuntime &runtime) const
    {
        return structure(query, [&runtime](const std::string &sys, const std::string &user,
                                           const std::string &grammar) {
            auto r = runtime.complete(sys, user, grammar);
            Completion c;
            c.ok = r.ok;
            c.text = std::move(r.text);
            c.error = std::move(r.error);
            return c;
        });
    }
}
