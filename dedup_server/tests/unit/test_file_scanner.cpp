#include <gtest/gtest.h>
#include <filesystem>
#include <vector>
#include <fstream>
#include <unistd.h>
#include "filesmanager/file_scanner.hpp"

namespace fs = std::filesystem;
using namespace MediaDedup::Files;

TEST(FileScannerTest, RootMissingEmitsOneError)
{
    std::vector<FileRecord> emitted;
    scan("/path/does/not/exist", FileScannerOptions{}, [&](const FileRecord &r)
         { emitted.push_back(r); });
    ASSERT_EQ(emitted.size(), 1u);
    EXPECT_TRUE(emitted[0].hasError());
}

TEST(FileScannerTest, EnumeratesNonRecursive)
{
    auto tmpdir = fs::temp_directory_path() / ("mds_test_" + std::to_string(::getpid()));
    fs::create_directories(tmpdir);
    auto a = tmpdir / "a.txt";
    auto b = tmpdir / "b.txt";
    std::ofstream(a.string()) << "a";
    std::ofstream(b.string()) << "b";
    std::vector<FileRecord> emitted;
    FileScannerOptions opt;
    opt.recursive = false;
    scan(tmpdir, opt, [&](const FileRecord &r)
         { if(!r.hasError()) emitted.push_back(r); });
    EXPECT_GE(emitted.size(), 2u);
    fs::remove_all(tmpdir);
}

#ifdef FILE_SCANNER_TEST_STANDALONE_MAIN
int main(int argc, char **argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
#endif
