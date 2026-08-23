#pragma once

#include "media_processors/face/face_detector.hpp"

namespace MediaDedup
{
    /**
     * @brief FACE-1: Apple Vision implementation of the detector seam.
     *
     * Objective-C++ (vision_face_detector.mm), compiled into the SERVER
     * target only — test binaries use fakes against FaceDetector, so
     * Vision/ImageIO never leak into CI.
     *
     * - loadImage: ImageIO thumbnail decode (handles JPEG/PNG/HEIC/TIFF and
     *   macOS-supported RAW), longest side capped, one decode shared by
     *   detection and alignment.
     * - detect: VNDetectFaceLandmarksRequest for boxes + landmark
     *   constellations (reduced to the 5-point ArcFace set), plus
     *   VNDetectFaceCaptureQualityRequest matched by box overlap for the
     *   quality score. Vision's bottom-left origin is flipped to top-left.
     */
    class VisionFaceDetector final : public FaceDetector
    {
    public:
        ImageBuffer loadImage(const std::string &file_path, int max_dim) override;
        std::vector<DetectedFace> detect(const ImageBuffer &image) override;
    };
}
