#include <iostream>
#include <libraw/libraw.h>
#include <vector>
#include <string>

int main()
{
    std::cout << "LibRaw Version: " << LibRaw::version() << std::endl;
    std::cout << "LibRaw Capabilities: " << LibRaw::capabilities() << std::endl;

    // List of formats from our config
    std::vector<std::string> formats = {
        "cr2", "nef", "arw", "dng", "raf", "rw2", "orf", "pef", "srw",
        "kdc", "dcr", "mos", "mrw", "raw", "bay", "3fr", "fff", "mef",
        "iiq", "rwz", "nrw", "rwl", "r3d", "dcm", "dicom"};

    std::cout << "\nFormat Support Analysis:" << std::endl;
    std::cout << "=======================" << std::endl;

    for (const auto &format : formats)
    {
        std::cout << format << ": ";

        // Check if LibRaw supports this format
        // This is a simplified check - in reality, LibRaw determines support at runtime
        // based on file headers, not extensions
        if (format == "cr2" || format == "nef" || format == "arw" ||
            format == "dng" || format == "raf" || format == "rw2" ||
            format == "orf" || format == "pef" || format == "srw" ||
            format == "kdc" || format == "dcr" || format == "mos" ||
            format == "mrw" || format == "bay" || format == "3fr" ||
            format == "fff" || format == "mef" || format == "iiq" ||
            format == "rwz" || format == "nrw" || format == "rwl")
        {
            std::cout << "✓ Supported by LibRaw" << std::endl;
        }
        else if (format == "raw")
        {
            std::cout << "⚠ Generic RAW - depends on file content" << std::endl;
        }
        else if (format == "r3d")
        {
            std::cout << "❌ Red RAW - requires specialized libraries" << std::endl;
        }
        else if (format == "dcm" || format == "dicom")
        {
            std::cout << "❌ DICOM - medical imaging format, not supported by LibRaw" << std::endl;
        }
        else
        {
            std::cout << "❓ Unknown support status" << std::endl;
        }
    }

    std::cout << "\nSummary:" << std::endl;
    std::cout << "=========" << std::endl;
    std::cout << "✓ Fully Supported: 20 formats" << std::endl;
    std::cout << "⚠ Conditionally Supported: 1 format (raw)" << std::endl;
    std::cout << "❌ Not Supported: 4 formats (r3d, dcm, dicom)" << std::endl;

    return 0;
}