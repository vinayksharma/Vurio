#include <libraw/libraw.h>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <iostream>
#include <string>
#include <filesystem>
#include <csignal>
#include <opencv2/opencv.hpp>

// Simple helper: raw_to_jpeg <input_raw> <output_jpeg>

static std::string g_current_file;

void sig_handler(int sig)
{
    std::cerr << "[raw_to_jpeg] Fatal signal " << sig << " while processing: " << g_current_file << std::endl;
    std::exit(128 + sig);
}

int main(int argc, char **argv)
{
    std::signal(SIGBUS, sig_handler);
    std::signal(SIGSEGV, sig_handler);

    if (argc < 3)
    {
        std::cerr << "Usage: raw_to_jpeg <input_raw> <output_jpeg>" << std::endl;
        return 2;
    }

    std::string input = argv[1];
    std::string output = argv[2];
    g_current_file = input;

    try
    {
        // Ensure parent dir exists
        std::filesystem::create_directories(std::filesystem::path(output).parent_path());

        LibRaw raw;
        raw.imgdata.params.use_camera_wb = 1;
        raw.imgdata.params.use_auto_wb = 0;
        raw.imgdata.params.no_auto_bright = 1;
        raw.imgdata.params.output_bps = 8;
        raw.imgdata.params.output_color = 1; // sRGB
        raw.imgdata.params.half_size = 0;

        int rc = raw.open_file(input.c_str());
        if (rc != LIBRAW_SUCCESS)
        {
            std::cerr << "open_file: " << libraw_strerror(rc) << " (" << rc << ")" << std::endl;
            return 3;
        }

        rc = raw.unpack();
        if (rc != LIBRAW_SUCCESS)
        {
            std::cerr << "unpack: " << libraw_strerror(rc) << " (" << rc << ")" << std::endl;
            raw.recycle();
            return 4;
        }

        rc = raw.dcraw_process();
        if (rc != LIBRAW_SUCCESS)
        {
            std::cerr << "dcraw_process: " << libraw_strerror(rc) << " (" << rc << ")" << std::endl;
            raw.recycle();
            return 5;
        }

        libraw_processed_image_t *img = raw.dcraw_make_mem_image(&rc);
        if (!img || rc != LIBRAW_SUCCESS)
        {
            std::cerr << "dcraw_make_mem_image: " << libraw_strerror(rc) << " (" << rc << ")" << std::endl;
            if (img)
                LibRaw::dcraw_clear_mem(img);
            raw.recycle();
            return 6;
        }

        // Expect 8-bit RGB data
        if (img->type != LIBRAW_IMAGE_BITMAP || img->colors != 3 || img->bits != 8)
        {
            std::cerr << "Unsupported image buffer (type=" << img->type << ", colors=" << img->colors << ", bits=" << img->bits << ")" << std::endl;
            LibRaw::dcraw_clear_mem(img);
            raw.recycle();
            return 7;
        }

        // Construct cv::Mat (RGB to BGR for OpenCV)
        cv::Mat rgb(img->height, img->width, CV_8UC3, img->data);
        cv::Mat bgr;
        cv::cvtColor(rgb, bgr, cv::COLOR_RGB2BGR);

        std::vector<int> params = {cv::IMWRITE_JPEG_QUALITY, 92};
        if (!cv::imwrite(output, bgr, params))
        {
            std::cerr << "imwrite failed: " << output << std::endl;
            LibRaw::dcraw_clear_mem(img);
            raw.recycle();
            return 8;
        }

        LibRaw::dcraw_clear_mem(img);
        raw.recycle();

        // Success - no need to output anything, transcoding manager will log the result
        return 0;
    }
    catch (const std::exception &e)
    {
        std::cerr << "Exception: " << e.what() << std::endl;
        return 9;
    }
}
