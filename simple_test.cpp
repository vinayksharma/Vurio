#include <iostream>
#include <vector>
#include <string>
#include <Magick++.h>

int main()
{
    std::cout << "Testing ImageMagick++ directly..." << std::endl;

    try
    {
        // Initialize Magick++
        Magick::InitializeMagick(nullptr);

        // Test with a simple image file
        std::string test_file = "/Users/vinaysharma/pictures/testset/sample.jpg";

        std::cout << "Reading image: " << test_file << std::endl;

        // Create independent Magick++ image instance
        Magick::Image image;

        // Read the image file
        image.read(test_file);

        std::cout << "Image read successfully!" << std::endl;
        std::cout << "Image size: " << image.columns() << "x" << image.rows() << std::endl;
        std::cout << "Image depth: " << image.depth() << " bits" << std::endl;
        std::cout << "Image format: " << image.magick() << std::endl;

        // Convert to 8-bit RGB if needed
        if (image.depth() > 8)
        {
            image.depth(8);
            std::cout << "Converted to 8-bit depth" << std::endl;
        }

        // Ensure RGB color space
        if (image.colorSpace() != Magick::RGBColorspace)
        {
            image.colorSpace(Magick::RGBColorspace);
            std::cout << "Converted to RGB color space" << std::endl;
        }

        // Set TIFF format and compression
        image.magick("TIFF");
        image.compressType(Magick::LZWCompression);

        // Write to memory buffer
        Magick::Blob blob;
        image.write(&blob);

        std::cout << "SUCCESS: Image transcoded to TIFF!" << std::endl;
        std::cout << "TIFF data size: " << blob.length() << " bytes" << std::endl;

        // Verify it's actually TIFF data
        const std::uint8_t *data = static_cast<const std::uint8_t *>(blob.data());
        if (blob.length() > 8)
        {
            bool is_little_endian = (data[0] == 'I' && data[1] == 'I');
            bool is_big_endian = (data[0] == 'M' && data[1] == 'M');
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
    catch (const Magick::Exception &e)
    {
        std::cout << "FAILED: Magick++ error: " << e.what() << std::endl;
        return 1;
    }
    catch (const std::exception &e)
    {
        std::cout << "FAILED: Standard error: " << e.what() << std::endl;
        return 1;
    }
    catch (...)
    {
        std::cout << "FAILED: Unknown error" << std::endl;
        return 1;
    }

    return 0;
}
