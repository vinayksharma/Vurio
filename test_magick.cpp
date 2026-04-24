#include <Magick++.h>
#include <iostream>

int main(int argc, char **argv) {
    Magick::InitializeMagick(*argv);
    Magick::Image img("100x200", "white");
    std::cout << "Original: " << img.columns() << "x" << img.rows() << std::endl;
    Magick::Geometry geom(128, 128);
    img.resize(geom);
    std::cout << "Resized geom(128, 128): " << img.columns() << "x" << img.rows() << std::endl;
    return 0;
}
