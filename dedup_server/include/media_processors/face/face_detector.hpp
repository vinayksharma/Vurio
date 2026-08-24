#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace MediaDedup
{
    /**
     * @brief FACE-1 detector seam (docs/todo_face_recognition.md §2).
     *
     * The production implementation is VisionFaceDetector (Objective-C++,
     * Apple Vision, server target only). Tests and any future
     * cross-platform build implement this interface instead — nothing
     * outside the .mm file touches Vision types.
     *
     * Coordinates are normalized [0,1] with a TOP-LEFT origin (Vision's
     * bottom-left results are flipped inside the detector).
     */
    struct FaceLandmarks5
    {
        // left eye, right eye, nose tip, left mouth corner, right mouth
        // corner — the ArcFace alignment template order.
        double x[5] = {0, 0, 0, 0, 0};
        double y[5] = {0, 0, 0, 0, 0};
    };

    struct DetectedFace
    {
        double bbox_x = 0, bbox_y = 0, bbox_w = 0, bbox_h = 0; // normalized
        FaceLandmarks5 landmarks;
        bool has_landmarks = false;
        double quality = 0; // capture quality (or confidence) 0..1
    };

    /// Decoded pixels shared by detection and alignment: one decode per
    /// file, both consumers. RGBA8, row-major, top-left origin.
    struct ImageBuffer
    {
        int width = 0;
        int height = 0;
        std::vector<std::uint8_t> rgba; // width*height*4

        bool valid() const
        {
            return width > 0 && height > 0 &&
                   rgba.size() == static_cast<std::size_t>(width) * height * 4;
        }
    };

    class FaceDetector
    {
    public:
        virtual ~FaceDetector() = default;

        /// Decode a file to RGBA, longest side capped at max_dim. Returns an
        /// invalid buffer on failure (unreadable/unsupported file).
        virtual ImageBuffer loadImage(const std::string &file_path, int max_dim) = 0;

        /// Detect faces in a previously loaded buffer.
        virtual std::vector<DetectedFace> detect(const ImageBuffer &image) = 0;
    };
}
