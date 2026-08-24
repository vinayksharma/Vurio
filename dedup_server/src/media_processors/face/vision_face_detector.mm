#include "media_processors/face/vision_face_detector.hpp"

#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>
#import <Vision/Vision.h>

#include <Poco/Logger.h>
#include <algorithm>

namespace MediaDedup
{
    namespace
    {
        Poco::Logger &visionLogger()
        {
            static Poco::Logger &logger = Poco::Logger::get("VisionFaceDetector");
            return logger;
        }

        // Median point of a Vision landmark region, converted to normalized
        // image coordinates (top-left origin).
        bool regionMedian(VNFaceLandmarkRegion2D *region, CGRect bbox,
                          double &out_x, double &out_y)
        {
            if (region == nil || region.pointCount == 0)
                return false;
            const CGPoint *pts = region.normalizedPoints;
            double sx = 0, sy = 0;
            for (NSUInteger i = 0; i < region.pointCount; ++i)
            {
                sx += pts[i].x;
                sy += pts[i].y;
            }
            const double lx = sx / region.pointCount;
            const double ly = sy / region.pointCount;
            // Landmark points are normalized WITHIN the face bbox
            // (bottom-left origin); lift to image space, then flip y.
            out_x = bbox.origin.x + lx * bbox.size.width;
            out_y = 1.0 - (bbox.origin.y + ly * bbox.size.height);
            return true;
        }

        // Leftmost/rightmost points of the outer-lips region: the mouth
        // corners of the 5-point ArcFace set.
        bool lipCorners(VNFaceLandmarkRegion2D *lips, CGRect bbox,
                        double &lx_out, double &ly_out, double &rx_out, double &ry_out)
        {
            if (lips == nil || lips.pointCount < 2)
                return false;
            const CGPoint *pts = lips.normalizedPoints;
            NSUInteger li = 0, ri = 0;
            for (NSUInteger i = 1; i < lips.pointCount; ++i)
            {
                if (pts[i].x < pts[li].x) li = i;
                if (pts[i].x > pts[ri].x) ri = i;
            }
            lx_out = bbox.origin.x + pts[li].x * bbox.size.width;
            ly_out = 1.0 - (bbox.origin.y + pts[li].y * bbox.size.height);
            rx_out = bbox.origin.x + pts[ri].x * bbox.size.width;
            ry_out = 1.0 - (bbox.origin.y + pts[ri].y * bbox.size.height);
            return true;
        }

        double iou(CGRect a, CGRect b)
        {
            const CGRect inter = CGRectIntersection(a, b);
            if (CGRectIsNull(inter))
                return 0.0;
            const double ia = inter.size.width * inter.size.height;
            const double ua = a.size.width * a.size.height +
                              b.size.width * b.size.height - ia;
            return ua > 0 ? ia / ua : 0.0;
        }
    }

    ImageBuffer VisionFaceDetector::loadImage(const std::string &file_path, int max_dim)
    {
        ImageBuffer out;
        @autoreleasepool
        {
            NSURL *url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:file_path.c_str()]];
            CGImageSourceRef src = CGImageSourceCreateWithURL((__bridge CFURLRef)url, nullptr);
            if (!src)
                return out;
            NSDictionary *opts = @{
                (id)kCGImageSourceCreateThumbnailFromImageAlways : @YES,
                (id)kCGImageSourceThumbnailMaxPixelSize : @(max_dim),
                (id)kCGImageSourceCreateThumbnailWithTransform : @YES, // EXIF orientation applied
            };
            CGImageRef img = CGImageSourceCreateThumbnailAtIndex(src, 0, (__bridge CFDictionaryRef)opts);
            CFRelease(src);
            if (!img)
                return out;

            const int w = static_cast<int>(CGImageGetWidth(img));
            const int h = static_cast<int>(CGImageGetHeight(img));
            out.width = w;
            out.height = h;
            out.rgba.assign(static_cast<std::size_t>(w) * h * 4, 0);

            CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
            CGContextRef ctx = CGBitmapContextCreate(out.rgba.data(), w, h, 8, w * 4, cs,
                                                     kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
            CGColorSpaceRelease(cs);
            if (!ctx)
            {
                CGImageRelease(img);
                out = ImageBuffer{};
                return out;
            }
            CGContextDrawImage(ctx, CGRectMake(0, 0, w, h), img);
            CGContextRelease(ctx);
            CGImageRelease(img);
        }
        return out;
    }

    std::vector<DetectedFace> VisionFaceDetector::detect(const ImageBuffer &image)
    {
        std::vector<DetectedFace> faces;
        if (!image.valid())
            return faces;

        @autoreleasepool
        {
            NSData *data = [NSData dataWithBytesNoCopy:const_cast<std::uint8_t *>(image.rgba.data())
                                                length:image.rgba.size()
                                          freeWhenDone:NO];
            CGDataProviderRef provider = CGDataProviderCreateWithCFData((__bridge CFDataRef)data);
            CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
            CGImageRef img = CGImageCreate(image.width, image.height, 8, 32, image.width * 4, cs,
                                           kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big,
                                           provider, nullptr, false, kCGRenderingIntentDefault);
            CGColorSpaceRelease(cs);
            CGDataProviderRelease(provider);
            if (!img)
                return faces;

            VNDetectFaceLandmarksRequest *landmarks = [[VNDetectFaceLandmarksRequest alloc] init];
            VNDetectFaceCaptureQualityRequest *quality = [[VNDetectFaceCaptureQualityRequest alloc] init];
            VNImageRequestHandler *handler =
                [[VNImageRequestHandler alloc] initWithCGImage:img options:@{}];
            NSError *error = nil;
            const bool ok = [handler performRequests:@[ landmarks, quality ] error:&error];
            CGImageRelease(img);
            if (!ok)
            {
                visionLogger().warning("Vision request failed: %s",
                                       std::string(error ? error.localizedDescription.UTF8String : "unknown"));
                return faces;
            }

            for (VNFaceObservation *obs in landmarks.results)
            {
                DetectedFace f;
                const CGRect bb = obs.boundingBox; // normalized, bottom-left origin
                f.bbox_x = bb.origin.x;
                f.bbox_y = 1.0 - bb.origin.y - bb.size.height; // flip to top-left
                f.bbox_w = bb.size.width;
                f.bbox_h = bb.size.height;
                f.quality = obs.confidence;

                VNFaceLandmarks2D *lm = obs.landmarks;
                if (lm != nil)
                {
                    FaceLandmarks5 &p = f.landmarks;
                    bool got = regionMedian(lm.leftPupil ?: lm.leftEye, bb, p.x[0], p.y[0]);
                    got = regionMedian(lm.rightPupil ?: lm.rightEye, bb, p.x[1], p.y[1]) && got;
                    got = regionMedian(lm.nose, bb, p.x[2], p.y[2]) && got;
                    got = lipCorners(lm.outerLips, bb, p.x[3], p.y[3], p.x[4], p.y[4]) && got;
                    // Vision reports pupils in IMAGE ordering (leftmost pupil
                    // first is not guaranteed); enforce left-of-right so the
                    // alignment template always matches.
                    if (got && p.x[0] > p.x[1])
                    {
                        std::swap(p.x[0], p.x[1]);
                        std::swap(p.y[0], p.y[1]);
                    }
                    if (got && p.x[3] > p.x[4])
                    {
                        std::swap(p.x[3], p.x[4]);
                        std::swap(p.y[3], p.y[4]);
                    }
                    f.has_landmarks = got;
                }

                // Capture quality (a better "is this a usable face" signal
                // than detector confidence): matched by box overlap.
                for (VNFaceObservation *q in quality.results)
                {
                    if (iou(bb, q.boundingBox) > 0.5 && q.faceCaptureQuality != nil)
                    {
                        f.quality = q.faceCaptureQuality.doubleValue;
                        break;
                    }
                }
                faces.push_back(f);
            }
        }
        return faces;
    }
}
