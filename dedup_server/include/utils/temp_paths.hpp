#pragma once

#include <string>

namespace MediaDedup
{
    /**
     * @brief Build a collision-free temp file path: <tmpdir>/<prefix>_<pid>_<seq>_<ns><ext>
     *
     * Replaces the previous shared-static mt19937 naming, which was raced from
     * multiple pool threads (UB) and could hand two generations the same file.
     * The atomic sequence + pid + monotonic nanoseconds cannot collide.
     */
    std::string makeTempFilePath(const std::string &prefix, const std::string &extension);

} // namespace MediaDedup
