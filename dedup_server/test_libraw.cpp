#include <libraw/libraw.h>
#include <iostream>
int main() {
    LibRaw rawProcessor;
    int ret = rawProcessor.open_file("test.cr2");
    std::cout << "Ret: " << ret << std::endl;
    return 0;
}
