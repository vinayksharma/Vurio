#include <iostream>
#include <chrono>
#include <vector>
#include <thread>
#include <Magick++.h>

int main()
{
    // Initialize ImageMagick once
    Magick::InitializeMagick(nullptr);

    const int iterations = 1000;
    const int num_threads = 4;

    std::cout << "Benchmarking ImageMagick instance creation/destruction..." << std::endl;
    std::cout << "Iterations per thread: " << iterations << std::endl;
    std::cout << "Number of threads: " << num_threads << std::endl;

    // Single-threaded benchmark
    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < iterations; ++i)
    {
        Magick::Image image;
        // Simulate some basic operations
        image.size("100x100");
        image.magick("RGB");
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto single_thread_time = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

    std::cout << "Single-threaded time: " << single_thread_time.count() << " microseconds" << std::endl;
    std::cout << "Average per instance: " << (single_thread_time.count() / iterations) << " microseconds" << std::endl;

    // Multi-threaded benchmark (without mutex)
    std::vector<std::thread> threads;
    std::vector<long> thread_times(num_threads);

    start = std::chrono::high_resolution_clock::now();

    for (int t = 0; t < num_threads; ++t)
    {
        threads.emplace_back([&thread_times, t, iterations]()
                             {
            auto thread_start = std::chrono::high_resolution_clock::now();
            
            for (int i = 0; i < iterations; ++i) {
                Magick::Image image;
                image.size("100x100");
                image.magick("RGB");
            }
            
            auto thread_end = std::chrono::high_resolution_clock::now();
            thread_times[t] = std::chrono::duration_cast<std::chrono::microseconds>(thread_end - thread_start).count(); });
    }

    for (auto &thread : threads)
    {
        thread.join();
    }

    end = std::chrono::high_resolution_clock::now();
    auto multi_thread_time = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

    long total_thread_time = 0;
    for (long time : thread_times)
    {
        total_thread_time += time;
    }

    std::cout << "Multi-threaded wall time: " << multi_thread_time.count() << " microseconds" << std::endl;
    std::cout << "Multi-threaded total CPU time: " << total_thread_time << " microseconds" << std::endl;
    std::cout << "Average per instance (multi-threaded): " << (total_thread_time / (iterations * num_threads)) << " microseconds" << std::endl;

    // Test with mutex (simulating current approach)
    std::mutex magick_mutex;
    threads.clear();
    thread_times.assign(num_threads, 0);

    start = std::chrono::high_resolution_clock::now();

    for (int t = 0; t < num_threads; ++t)
    {
        threads.emplace_back([&thread_times, &magick_mutex, t, iterations]()
                             {
            auto thread_start = std::chrono::high_resolution_clock::now();
            
            for (int i = 0; i < iterations; ++i) {
                std::lock_guard<std::mutex> lock(magick_mutex);
                Magick::Image image;
                image.size("100x100");
                image.magick("RGB");
            }
            
            auto thread_end = std::chrono::high_resolution_clock::now();
            thread_times[t] = std::chrono::duration_cast<std::chrono::microseconds>(thread_end - thread_start).count(); });
    }

    for (auto &thread : threads)
    {
        thread.join();
    }

    end = std::chrono::high_resolution_clock::now();
    auto mutex_time = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

    total_thread_time = 0;
    for (long time : thread_times)
    {
        total_thread_time += time;
    }

    std::cout << "Mutex-serialized wall time: " << mutex_time.count() << " microseconds" << std::endl;
    std::cout << "Mutex-serialized total CPU time: " << total_thread_time << " microseconds" << std::endl;
    std::cout << "Average per instance (mutex): " << (total_thread_time / (iterations * num_threads)) << " microseconds" << std::endl;

    std::cout << "\nPerformance comparison:" << std::endl;
    std::cout << "Multi-threaded speedup: " << (double(mutex_time.count()) / multi_thread_time.count()) << "x" << std::endl;

    return 0;
}
