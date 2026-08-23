#pragma once

#include "media_processors/face/face_detector.hpp"

#include <array>
#include <vector>

namespace MediaDedup
{
    /**
     * @brief FACE-1 alignment: 5-point similarity transform onto the
     * standard ArcFace 112×112 template, then a bilinear warp of the
     * source RGBA buffer to a 112×112 RGB crop.
     *
     * The transform math (Umeyama, similarity = scale+rotation+translation)
     * is exposed separately so it is unit-testable without pixels.
     */
    class FaceAlign
    {
    public:
        static constexpr int kSize = 112;

        /// The ArcFace reference landmark template in 112×112 pixel space:
        /// left eye, right eye, nose tip, left mouth, right mouth.
        static const std::array<double, 5> &templateX();
        static const std::array<double, 5> &templateY();

        /// Least-squares similarity transform mapping src (image pixel
        /// coordinates) onto the template. Returns {a, b, tx, ty} for
        ///   x' = a*x - b*y + tx
        ///   y' = b*x + a*y + ty
        /// (scale*rotation + translation). False when the points are
        /// degenerate (collinear/coincident).
        static bool similarityTransform(const std::array<double, 5> &src_x,
                                        const std::array<double, 5> &src_y,
                                        std::array<double, 4> &out);

        /// Align one detected face out of the decoded buffer: landmarks
        /// (normalized) are scaled to pixels, the transform is estimated,
        /// and the buffer is warped to a kSize×kSize RGB (3-channel,
        /// interleaved, row-major) crop. False when landmarks are missing
        /// or degenerate.
        static bool alignFace(const ImageBuffer &image, const DetectedFace &face,
                              std::vector<std::uint8_t> &out_rgb);
    };
}
