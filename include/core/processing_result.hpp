#pragma once

#include <string>
#include <vector>

/**
 * @brief Media artifact data structure
 */
struct MediaArtifact
{
    std::vector<uint8_t> data;
    std::string format;
    std::string hash;
    double confidence;
    std::string metadata;

    MediaArtifact() : confidence(0.0) {}
    MediaArtifact(const std::vector<uint8_t> &d, const std::string &f = "", const std::string &h = "", double c = 0.0, const std::string &m = "")
        : data(d), format(f), hash(h), confidence(c), metadata(m) {}
};

/**
 * @brief Media processing result
 */
struct ProcessingResult
{
    bool success;
    std::string error_message;
    MediaArtifact artifact;

    ProcessingResult() : success(false) {}
    ProcessingResult(bool s, const std::string &msg = "")
        : success(s), error_message(msg) {}
};
