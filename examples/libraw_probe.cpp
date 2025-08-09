#include <libraw/libraw.h>
#include <filesystem>
#include <iostream>
#include <vector>
#include <string>
#include <algorithm>
#include <csignal>
#include <cstdlib>
#include <thread>
#include <mutex>
#include <atomic>
#include <random>

namespace fs = std::filesystem;

static std::string g_current_file;
static std::mutex g_libraw_mutex;

void sig_handler(int sig)
{
    std::cerr << "\n[FATAL] Received signal " << sig << " while processing: " << g_current_file << std::endl;
    std::exit(128 + sig);
}

static bool has_raw_ext(const std::string &path)
{
    static const std::vector<std::string> exts = {
        "cr2", "cr3", "nef", "arw", "raf", "dng", "rw2", "orf", "pef", "srw", "kdc", "dcr"};
    auto pos = path.find_last_of('.');
    if (pos == std::string::npos)
        return false;
    std::string ext = path.substr(pos + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return std::find(exts.begin(), exts.end(), ext) != exts.end();
}

int main(int argc, char **argv)
{
    // Install basic signal handlers to catch hard crashes (e.g., SIGBUS)
    std::signal(SIGBUS, sig_handler);
    std::signal(SIGSEGV, sig_handler);

    std::string mode = "process"; // open | unpack | process
    std::string dir;
    int threads = 1;
    size_t limit = 0;
    bool shuffle = false;
    bool use_global_mutex = false;
    std::string write_dir;

    if (argc >= 2)
    {
        dir = argv[1];
    }
    else
    {
        const char *home = std::getenv("HOME");
        if (!home)
        {
            std::cerr << "HOME not set; please pass directory as first argument" << std::endl;
            return 2;
        }
        dir = std::string(home) + "/Pictures/raw images";
    }
    if (argc >= 3)
    {
        mode = argv[2];
    }

    // Optional flags
    for (int i = 3; i < argc; ++i)
    {
        std::string a = argv[i];
        auto next = [&](int &i) -> const char *
        {
            if (i + 1 < argc)
                return argv[++i];
            std::cerr << "Missing value for " << a << std::endl;
            std::exit(2);
        };
        if (a == "--threads")
        {
            threads = std::max(1, std::atoi(next(i)));
        }
        else if (a == "--limit")
        {
            limit = static_cast<size_t>(std::max(0, std::atoi(next(i))));
        }
        else if (a == "--shuffle")
        {
            shuffle = true;
        }
        else if (a == "--global-mutex")
        {
            use_global_mutex = true;
        }
        else if (a == "--write")
        {
            write_dir = next(i);
        }
        else if (a == "--help" || a == "-h")
        {
            std::cout << "Usage: libraw_probe [dir] [mode=open|unpack|process] [--threads N] [--limit M] [--shuffle] [--global-mutex] [--write OUTDIR]\n";
            return 0;
        }
    }

    std::cout << "LibRaw probe" << std::endl;
    std::cout << "LibRaw version: " << LibRaw::version() << std::endl;
    std::cout << "Directory: " << dir << std::endl;
    std::cout << "Mode: " << mode << " (open|unpack|process)" << std::endl;
    std::cout << "Threads: " << threads << (use_global_mutex ? " (global LibRaw mutex)" : "") << std::endl;
    if (limit)
        std::cout << "Limit: " << limit << std::endl;
    if (!write_dir.empty())
        std::cout << "Write dir: " << write_dir << std::endl;

    size_t tested = 0, opened = 0, unpacked = 0, processed = 0, failed = 0;

    // Collect files
    std::vector<std::string> files;
    try
    {
        for (auto it = fs::recursive_directory_iterator(dir); it != fs::recursive_directory_iterator(); ++it)
        {
            if (!it->is_regular_file())
                continue;
            const std::string path = it->path().string();
            if (!has_raw_ext(path))
                continue;
            files.push_back(path);
        }
    }
    catch (const std::exception &e)
    {
        std::cerr << "Iterator exception: " << e.what() << std::endl;
        return 3;
    }

    if (shuffle)
    {
        std::mt19937 rng{std::random_device{}()};
        std::shuffle(files.begin(), files.end(), rng);
    }
    if (limit && files.size() > limit)
        files.resize(limit);

    std::mutex cout_mutex;
    std::atomic<size_t> index{0};

    auto worker = [&](int tid)
    {
        for (;;)
        {
            size_t i = index.fetch_add(1);
            if (i >= files.size())
                return;
            const std::string path = files[i];
            g_current_file = path;

            {
                std::lock_guard<std::mutex> lk(cout_mutex);
                std::cout << "\n[FILE][T" << tid << "] " << path << std::endl;
            }

            try
            {
                if (use_global_mutex)
                    g_libraw_mutex.lock();
                LibRaw raw;
                // Conservative params to avoid risky paths
                raw.imgdata.params.use_camera_wb = 1;
                raw.imgdata.params.use_auto_wb = 0;
                raw.imgdata.params.no_auto_bright = 1;
                raw.imgdata.params.output_bps = 8;
                raw.imgdata.params.output_color = 1; // sRGB
                raw.imgdata.params.half_size = 0;

                int rc = raw.open_file(path.c_str());
                if (use_global_mutex)
                    g_libraw_mutex.unlock();
                if (rc != LIBRAW_SUCCESS)
                {
                    std::lock_guard<std::mutex> lk(cout_mutex);
                    std::cerr << "  open_file: " << libraw_strerror(rc) << " (" << rc << ")" << std::endl;
                    failed++;
                    continue;
                }
                opened++;
                {
                    std::lock_guard<std::mutex> lk(cout_mutex);
                    std::cout << "  opened: " << raw.imgdata.sizes.width << "x" << raw.imgdata.sizes.height << std::endl;
                }
                tested++;

                if (mode == "open")
                {
                    raw.recycle();
                    continue;
                }

                if (use_global_mutex)
                    g_libraw_mutex.lock();
                rc = raw.unpack();
                if (use_global_mutex)
                    g_libraw_mutex.unlock();
                if (rc != LIBRAW_SUCCESS)
                {
                    std::lock_guard<std::mutex> lk(cout_mutex);
                    std::cerr << "  unpack: " << libraw_strerror(rc) << " (" << rc << ")" << std::endl;
                    failed++;
                    raw.recycle();
                    continue;
                }
                unpacked++;
                {
                    std::lock_guard<std::mutex> lk(cout_mutex);
                    std::cout << "  unpack: ok" << std::endl;
                }

                if (mode == "unpack")
                {
                    raw.recycle();
                    continue;
                }

                if (use_global_mutex)
                    g_libraw_mutex.lock();
                rc = raw.dcraw_process();
                if (use_global_mutex)
                    g_libraw_mutex.unlock();
                if (rc != LIBRAW_SUCCESS)
                {
                    std::lock_guard<std::mutex> lk(cout_mutex);
                    std::cerr << "  dcraw_process: " << libraw_strerror(rc) << " (" << rc << ")" << std::endl;
                    failed++;
                    raw.recycle();
                    continue;
                }
                processed++;
                {
                    std::lock_guard<std::mutex> lk(cout_mutex);
                    std::cout << "  process: ok" << std::endl;
                }

                if (!write_dir.empty())
                {
                    try
                    {
                        fs::create_directories(write_dir);
                        auto out_name = fs::path(write_dir) / (fs::path(path).filename().string() + ".jpg");
                        if (use_global_mutex)
                            g_libraw_mutex.lock();
                        int wrc = raw.dcraw_ppm_tiff_writer(out_name.string().c_str());
                        if (use_global_mutex)
                            g_libraw_mutex.unlock();
                        std::lock_guard<std::mutex> lk(cout_mutex);
                        if (wrc == LIBRAW_SUCCESS)
                        {
                            std::cout << "  wrote: " << out_name << std::endl;
                        }
                        else
                        {
                            std::cerr << "  write: " << libraw_strerror(wrc) << " (" << wrc << ")" << std::endl;
                        }
                    }
                    catch (const std::exception &e)
                    {
                        std::lock_guard<std::mutex> lk(cout_mutex);
                        std::cerr << "  write exception: " << e.what() << std::endl;
                    }
                }

                raw.recycle();
            }
            catch (const std::exception &e)
            {
                std::lock_guard<std::mutex> lk(cout_mutex);
                std::cerr << "  EXCEPTION: " << e.what() << std::endl;
                failed++;
            }
            catch (...)
            {
                std::lock_guard<std::mutex> lk(cout_mutex);
                std::cerr << "  UNKNOWN EXCEPTION" << std::endl;
                failed++;
            }
        }
    };

    std::vector<std::thread> pool;
    for (int i = 0; i < threads; ++i)
        pool.emplace_back(worker, i);
    for (auto &t : pool)
        t.join();

    std::cout << "\nSummary:" << std::endl;
    std::cout << "  tested:   " << tested << std::endl;
    std::cout << "  opened:   " << opened << std::endl;
    std::cout << "  unpacked: " << unpacked << std::endl;
    std::cout << "  processed:" << processed << std::endl;
    std::cout << "  failed:   " << failed << std::endl;

    return 0;
}
