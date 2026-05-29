#include "utils/thumbnail_generator.hpp"
#include <iostream>
#include <Magick++.h>

using namespace MediaDedup;

int main() {
    Magick::InitializeMagick(nullptr);
    Magick::Blob blob;
    bool success = ThumbnailGenerator::generateToBlob(
        "/Users/vinaysharma/developer/vurio/dedup_server/tests/test_data/pictures/testset/test.arw", 
        256, blob, 85);
    std::cout << "Success: " << success << std::endl;
    std::cout << "Blob length: " << blob.length() << std::endl;
    return 0;
}
