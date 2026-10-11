// CHAIN-5 (docs/todo_tool_chain.md §5.6): Apple Vision salient-subject
// lifting (VNGenerateForegroundInstanceMaskRequest) as the macOS backend of
// the `subject` mask source. Objective-C++, compiled into the SERVER target
// only and registered by the initializer; the portable fallback (CLIPSeg
// "the main subject") stands behind it everywhere else (decision §13.17).

#include "adjustments/vision_subject_mask.hpp"
#include "adjustments/mask_detectors.hpp"

#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <Vision/Vision.h>

#include <Poco/Logger.h>
#include <opencv2/imgproc.hpp>

namespace MediaDedup::Adjustments
{
    bool visionSubjectMask(const cv::Mat &base_bgr8, cv::Mat &out)
    {
        if (base_bgr8.empty())
            return false;
        if (@available(macOS 14.0, *))
        {
            @autoreleasepool
            {
                cv::Mat rgba;
                cv::cvtColor(base_bgr8, rgba, cv::COLOR_BGR2RGBA);
                CVPixelBufferRef buffer = nullptr;
                NSDictionary *attrs = @{(id)kCVPixelBufferCGImageCompatibilityKey : @YES};
                CVReturn cr = CVPixelBufferCreate(kCFAllocatorDefault, rgba.cols, rgba.rows, kCVPixelFormatType_32BGRA,
                                                  (__bridge CFDictionaryRef)attrs, &buffer);
                if (cr != kCVReturnSuccess || !buffer)
                    return false;
                CVPixelBufferLockBaseAddress(buffer, 0);
                {
                    // Vision wants BGRA; convert straight into the buffer.
                    cv::Mat dst(rgba.rows, rgba.cols, CV_8UC4, CVPixelBufferGetBaseAddress(buffer),
                                CVPixelBufferGetBytesPerRow(buffer));
                    cv::cvtColor(base_bgr8, dst, cv::COLOR_BGR2BGRA);
                }
                CVPixelBufferUnlockBaseAddress(buffer, 0);

                VNGenerateForegroundInstanceMaskRequest *request = [[VNGenerateForegroundInstanceMaskRequest alloc] init];
                VNImageRequestHandler *handler = [[VNImageRequestHandler alloc] initWithCVPixelBuffer:buffer options:@{}];
                NSError *error = nil;
                const bool ok = [handler performRequests:@[ request ] error:&error];
                if (!ok || request.results.count == 0)
                {
                    CVPixelBufferRelease(buffer);
                    if (!ok)
                        Poco::Logger::get("VisionSubjectMask")
                            .warning("Vision request failed: %s",
                                     std::string(error ? error.localizedDescription.UTF8String : "unknown"));
                    return false;
                }
                VNInstanceMaskObservation *obs = request.results.firstObject;
                NSIndexSet *all = obs.allInstances;
                if (all.count == 0)
                {
                    CVPixelBufferRelease(buffer);
                    return false;
                }
                CVPixelBufferRef mask = [obs generateScaledMaskForImageForInstances:all fromRequestHandler:handler error:&error];
                CVPixelBufferRelease(buffer);
                if (!mask)
                {
                    Poco::Logger::get("VisionSubjectMask")
                        .warning("Vision mask generation failed: %s",
                                 std::string(error ? error.localizedDescription.UTF8String : "unknown"));
                    return false;
                }
                CVPixelBufferLockBaseAddress(mask, kCVPixelBufferLock_ReadOnly);
                const int w = static_cast<int>(CVPixelBufferGetWidth(mask));
                const int h = static_cast<int>(CVPixelBufferGetHeight(mask));
                const OSType fmt = CVPixelBufferGetPixelFormatType(mask);
                cv::Mat coverage;
                if (fmt == kCVPixelFormatType_OneComponent32Float)
                {
                    cv::Mat view(h, w, CV_32FC1, CVPixelBufferGetBaseAddress(mask), CVPixelBufferGetBytesPerRow(mask));
                    coverage = view.clone();
                }
                else if (fmt == kCVPixelFormatType_OneComponent8)
                {
                    cv::Mat view(h, w, CV_8UC1, CVPixelBufferGetBaseAddress(mask), CVPixelBufferGetBytesPerRow(mask));
                    view.convertTo(coverage, CV_32F, 1.0 / 255.0);
                }
                CVPixelBufferUnlockBaseAddress(mask, kCVPixelBufferLock_ReadOnly);
                CVPixelBufferRelease(mask);
                if (coverage.empty())
                    return false;
                if (coverage.size() != base_bgr8.size())
                    cv::resize(coverage, coverage, base_bgr8.size(), 0, 0, cv::INTER_LINEAR);
                cv::min(coverage, 1.0f, coverage);
                cv::max(coverage, 0.0f, coverage);
                out = coverage;
                return true;
            }
        }
        return false;
    }

    namespace
    {
        // Registered when this TU is linked (the server target on macOS).
        const bool kRegistered = [] {
            MaskDetectors::setSubjectBackend(&visionSubjectMask);
            return true;
        }();
    }
}
