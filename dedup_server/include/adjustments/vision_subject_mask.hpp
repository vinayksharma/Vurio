#pragma once

// CHAIN-5: the Apple Vision subject backend (vision_subject_mask.mm, server
// target on macOS only). The initializer registers it with
// MaskDetectors::setSubjectBackend; nothing else calls it directly.

#include <opencv2/core.hpp>

namespace MediaDedup::Adjustments
{
    // Coverage (CV_32FC1, base size) of the salient foreground instances,
    // or false when Vision finds none or is unavailable.
    bool visionSubjectMask(const cv::Mat &base_bgr8, cv::Mat &out);
}
