#include <iostream>
#include <libraw/libraw.h>
#include <filesystem>

int main()
{
    std::string raf_file = "/Volumes/truenas._smb._tcp.local/Images Aft 20190701/2020/2020-04-19/DSCF4997.RAF";

    if (!std::filesystem::exists(raf_file))
    {
        std::cerr << "RAF file not found: " << raf_file << std::endl;
        return 1;
    }

    std::cout << "Testing LibRaw with RAF file: " << raf_file << std::endl;

    LibRaw raw_processor;

    // Open the file
    int result = raw_processor.open_file(raf_file.c_str());
    if (result != LIBRAW_SUCCESS)
    {
        std::cerr << "Failed to open file: " << result << std::endl;
        return 1;
    }
    std::cout << "✓ File opened successfully" << std::endl;

    // Unpack the raw data
    result = raw_processor.unpack();
    if (result != LIBRAW_SUCCESS)
    {
        std::cerr << "Failed to unpack: " << result << std::endl;
        raw_processor.recycle();
        return 1;
    }
    std::cout << "✓ Raw data unpacked successfully" << std::endl;

    // Process the image
    result = raw_processor.dcraw_process();
    if (result != LIBRAW_SUCCESS)
    {
        std::cerr << "Failed to process: " << result << std::endl;
        raw_processor.recycle();
        return 1;
    }
    std::cout << "✓ Image processed successfully" << std::endl;

    // Write output
    std::string output_file = "/tmp/test_raf_output.jpg";
    result = raw_processor.dcraw_ppm_tiff_writer(output_file.c_str());
    if (result != LIBRAW_SUCCESS)
    {
        std::cerr << "Failed to write output: " << result << std::endl;
        raw_processor.recycle();
        return 1;
    }
    std::cout << "✓ Output written successfully: " << output_file << std::endl;

    raw_processor.recycle();

    if (std::filesystem::exists(output_file))
    {
        std::cout << "✓ Output file exists and size: " << std::filesystem::file_size(output_file) << " bytes" << std::endl;
    }

    std::cout << "LibRaw RAF test completed successfully!" << std::endl;
    return 0;
}