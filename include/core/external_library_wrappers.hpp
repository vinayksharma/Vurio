#pragma once
#include <memory>
#include <functional>
#include <opencv2/core.hpp>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libraw/libraw.h>

// RAII wrapper for FFmpeg AVFormatContext
class AVFormatContextRAII
{
private:
    AVFormatContext *ctx_;
    std::function<void(AVFormatContext **)> cleanup_func_;

public:
    AVFormatContextRAII() : ctx_(nullptr), cleanup_func_(avformat_close_input) {}
    ~AVFormatContextRAII()
    {
        if (ctx_)
            cleanup_func_(&ctx_);
    }

    AVFormatContext *get() { return ctx_; }
    AVFormatContext **address() { return &ctx_; }

    // Disable copy
    AVFormatContextRAII(const AVFormatContextRAII &) = delete;
    AVFormatContextRAII &operator=(const AVFormatContextRAII &) = delete;

    // Allow move
    AVFormatContextRAII(AVFormatContextRAII &&other) noexcept
        : ctx_(other.ctx_), cleanup_func_(other.cleanup_func_)
    {
        other.ctx_ = nullptr;
    }
};

// RAII wrapper for FFmpeg AVCodecContext
class AVCodecContextRAII
{
private:
    AVCodecContext *ctx_;

public:
    AVCodecContextRAII() : ctx_(nullptr) {}

    // Constructor that takes an existing context
    AVCodecContextRAII(AVCodecContext *existing_ctx) : ctx_(existing_ctx) {}

    ~AVCodecContextRAII()
    {
        if (ctx_)
            avcodec_free_context(&ctx_);
    }

    AVCodecContext *get() { return ctx_; }
    AVCodecContext **address() { return &ctx_; }

    // Method to set context from allocation
    void set(AVCodecContext *new_ctx)
    {
        if (ctx_)
            avcodec_free_context(&ctx_);
        ctx_ = new_ctx;
    }

    // Disable copy
    AVCodecContextRAII(const AVCodecContextRAII &) = delete;
    AVCodecContextRAII &operator=(const AVCodecContextRAII &) = delete;

    // Allow move
    AVCodecContextRAII(AVCodecContextRAII &&other) noexcept : ctx_(other.ctx_)
    {
        other.ctx_ = nullptr;
    }
};

// RAII wrapper for FFmpeg AVFrame
class AVFrameRAII
{
private:
    AVFrame *frame_;

public:
    AVFrameRAII() : frame_(nullptr) {}

    // Constructor that takes an existing frame
    AVFrameRAII(AVFrame *existing_frame) : frame_(existing_frame) {}

    ~AVFrameRAII()
    {
        if (frame_)
            av_frame_free(&frame_);
    }

    AVFrame *get() { return frame_; }
    AVFrame **address() { return &frame_; }

    // Method to set frame from allocation
    void set(AVFrame *new_frame)
    {
        if (frame_)
            av_frame_free(&frame_);
        frame_ = new_frame;
    }

    // Disable copy
    AVFrameRAII(const AVFrameRAII &) = delete;
    AVFrameRAII &operator=(const AVFrameRAII &) = delete;

    // Allow move
    AVFrameRAII(AVFrameRAII &&other) noexcept : frame_(other.frame_)
    {
        other.frame_ = nullptr;
    }
};

// RAII wrapper for FFmpeg AVPacket
class AVPacketRAII
{
private:
    AVPacket *packet_;

public:
    AVPacketRAII() : packet_(nullptr) {}

    // Constructor that takes an existing packet
    AVPacketRAII(AVPacket *existing_packet) : packet_(existing_packet) {}

    ~AVPacketRAII()
    {
        if (packet_)
            av_packet_free(&packet_);
    }

    AVPacket *get() { return packet_; }
    AVPacket **address() { return &packet_; }

    // Method to set packet from allocation
    void set(AVPacket *new_packet)
    {
        if (packet_)
            av_packet_free(&packet_);
        packet_ = new_packet;
    }

    // Disable copy
    AVPacketRAII(const AVPacketRAII &) = delete;
    AVPacketRAII &operator=(const AVPacketRAII &) = delete;

    // Allow move
    AVPacketRAII(AVPacketRAII &&other) noexcept : packet_(other.packet_)
    {
        other.packet_ = nullptr;
    }
};

// RAII wrapper for FFmpeg SwsContext
class SwsContextRAII
{
private:
    SwsContext *ctx_;

public:
    SwsContextRAII() : ctx_(nullptr) {}
    ~SwsContextRAII()
    {
        if (ctx_)
            sws_freeContext(ctx_);
    }

    SwsContext *get() { return ctx_; }
    void set(SwsContext *c) { ctx_ = c; }

    // Disable copy
    SwsContextRAII(const SwsContextRAII &) = delete;
    SwsContextRAII &operator=(const SwsContextRAII &) = delete;

    // Allow move
    SwsContextRAII(SwsContextRAII &&other) noexcept : ctx_(other.ctx_)
    {
        other.ctx_ = nullptr;
    }
};

// RAII wrapper for OpenCV Mat with automatic memory management
class OpenCVMatRAII
{
private:
    cv::Mat *mat_;

public:
    OpenCVMatRAII() : mat_(nullptr) {}
    ~OpenCVMatRAII()
    {
        if (mat_)
            delete mat_;
    }

    cv::Mat *get() { return mat_; }
    void set(cv::Mat *m) { mat_ = m; }

    // Disable copy
    OpenCVMatRAII(const OpenCVMatRAII &) = delete;
    OpenCVMatRAII &operator=(const OpenCVMatRAII &) = delete;
};

// RAII wrapper for LibRaw (already exists but enhanced)
class LibRawRAII
{
private:
    LibRaw *raw_;
    libraw_processed_image_t *img_;

public:
    LibRawRAII() : raw_(nullptr), img_(nullptr) {}

    ~LibRawRAII() { cleanup(); }

    // Disable copy constructor and assignment
    LibRawRAII(const LibRawRAII &) = delete;
    LibRawRAII &operator=(const LibRawRAII &) = delete;

    // Allow move constructor and assignment
    LibRawRAII(LibRawRAII &&other) noexcept : raw_(other.raw_), img_(other.img_)
    {
        other.raw_ = nullptr;
        other.img_ = nullptr;
    }

    LibRawRAII &operator=(LibRawRAII &&other) noexcept
    {
        if (this != &other)
        {
            cleanup();
            raw_ = other.raw_;
            img_ = other.img_;
            other.raw_ = nullptr;
            other.img_ = nullptr;
        }
        return *this;
    }

    void cleanup()
    {
        if (img_)
        {
            try
            {
                LibRaw::dcraw_clear_mem(img_);
            }
            catch (...)
            {
                // Ignore cleanup errors
            }
            img_ = nullptr;
        }

        if (raw_)
        {
            try
            {
                raw_->recycle();
                delete raw_;
            }
            catch (...)
            {
                // Ignore cleanup errors
            }
            raw_ = nullptr;
        }
    }

    LibRaw *getRaw() { return raw_; }
    libraw_processed_image_t *getImg() { return img_; }
    void setRaw(LibRaw *r) { raw_ = r; }
    void setImg(libraw_processed_image_t *i) { img_ = i; }
};
