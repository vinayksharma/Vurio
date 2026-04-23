#include <iostream>
#include <Poco/Format.h>
#include <string>

int main()
{
    const char* sqlite_err = "database is locked";
    std::cout << Poco::format("VectorIndex::insert step: %s", sqlite_err) << std::endl;
    return 0;
}
