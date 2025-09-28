#include <iostream>
#include <vector>
#include <string>
#include "include/media_processors/image/backends/image_magick_adapter.hpp"

int main()
{
    std::cout << "Testing ImageMagickAdapter..." << std::endl;

    // Test with a simple image file
    std::string test_file = "/Users/vinaysharma/pictures/testset/sample.jpg";
    std::vector<std::uint8_t> tiff_data;

    bool result = MediaDedup::ImageMagickAdapter::TranscodeToTiff(test_file, tiff_data);

    if (result)
    {
        std::cout << "SUCCESS: Transcoding worked!" << std::endl;
        std::cout << "TIFF data size: " << tiff_data.size() << " bytes" << std::endl;

        // Verify it's actually TIFF data
        if (tiff_data.size() > 8)
        {
            bool is_little_endian = (tiff_data[0] == 'I' && tiff_data[1] == 'I');
            bool is_big_endian = (tiff_data[0] == 'M' && tiff_data[1] == 'M');
            if (is_little_endian || is_big_endian)
            {
                std::cout << "SUCCESS: Data appears to be valid TIFF format" << std::endl;
            }
            else
            {
                std::cout << "WARNING: Data doesn't appear to be TIFF format" << std::endl;
            }
        }
    }
    else
    {
        std::cout << "FAILED: Transcoding failed" << std::endl;
    }

    return 0;
}
