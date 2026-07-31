#include "utils/temp_paths.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <unistd.h>

namespace MediaDedup
{
    std::string makeTempFilePath(const std::string &prefix, const std::string &extension)
    {
        static std::atomic<uint64_t> sequence{0};

        const uint64_t seq = sequence.fetch_add(1, std::memory_order_relaxed);
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();

        const std::string name = prefix + "_" + std::to_string(::getpid()) + "_" +
                                 std::to_string(seq) + "_" + std::to_string(ns) + extension;
        return (std::filesystem::temp_directory_path() / name).string();
    }

} // namespace MediaDedup
