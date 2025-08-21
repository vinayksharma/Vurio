#pragma once
#include <vector>
#include <mutex>
#include <memory>
#include <cstddef>
#include <algorithm>
#include <iomanip>
#include <sstream>
#include "logging/logger.hpp"

template <typename T>
class MemoryPool
{
private:
    struct PoolBlock
    {
        std::unique_ptr<T[]> data;
        size_t size;
        size_t used;

        PoolBlock(size_t block_size) : data(std::make_unique<T[]>(block_size)), size(block_size), used(0) {}
    };

    std::vector<std::unique_ptr<PoolBlock>> pools_;
    std::mutex pool_mutex_;
    size_t current_pool_;
    size_t initial_pool_size_;
    size_t growth_factor_;

public:
    MemoryPool(size_t initial_pool_size = 1024, size_t growth_factor = 2)
        : current_pool_(0), initial_pool_size_(initial_pool_size), growth_factor_(growth_factor)
    {
        addPool(initial_pool_size);
    }

    T *allocate(size_t count)
    {
        std::lock_guard<std::mutex> lock(pool_mutex_);

        // Check if current pool has enough space
        if (current_pool_ < pools_.size() &&
            pools_[current_pool_]->used + count <= pools_[current_pool_]->size)
        {
            T *ptr = &pools_[current_pool_]->data[pools_[current_pool_]->used];
            pools_[current_pool_]->used += count;
            return ptr;
        }

        // Move to next pool or create new one
        current_pool_++;
        if (current_pool_ >= pools_.size())
        {
            size_t new_size = pools_.empty() ? initial_pool_size_ : pools_.back()->size * growth_factor_;
            addPool(new_size);
        }

        // Reset usage for new pool
        pools_[current_pool_]->used = count;
        return &pools_[current_pool_]->data[0];
    }

    void reset()
    {
        std::lock_guard<std::mutex> lock(pool_mutex_);
        current_pool_ = 0;
        for (auto &pool : pools_)
        {
            pool->used = 0;
        }
    }

    size_t getCurrentPoolIndex() const { return current_pool_; }
    size_t getPoolCount() const { return pools_.size(); }
    size_t getTotalAllocated() const
    {
        size_t total = 0;
        for (const auto &pool : pools_)
        {
            total += pool->used;
        }
        return total;
    }

    void shrinkToFit()
    {
        std::lock_guard<std::mutex> lock(pool_mutex_);
        if (pools_.size() > 1)
        {
            pools_.resize(1);
            current_pool_ = 0;
            pools_[0]->used = 0;
        }
    }

private:
    void addPool(size_t size)
    {
        pools_.emplace_back(std::make_unique<PoolBlock>(size));
        Logger::debug("Added memory pool with size " + std::to_string(size) +
                      " (total pools: " + std::to_string(pools_.size()) + ")");
    }
};

// Specialized memory pool for common types
class CommonMemoryPools
{
private:
    static std::unique_ptr<MemoryPool<uint8_t>> uint8_pool_;
    static std::unique_ptr<MemoryPool<uint16_t>> uint16_pool_;
    static std::unique_ptr<MemoryPool<uint32_t>> uint32_pool_;
    static std::unique_ptr<MemoryPool<float>> float_pool_;
    static std::unique_ptr<MemoryPool<double>> double_pool_;
    static std::mutex pools_mutex_;

public:
    static void initialize(size_t initial_size = 1024)
    {
        std::lock_guard<std::mutex> lock(pools_mutex_);
        uint8_pool_ = std::make_unique<MemoryPool<uint8_t>>(initial_size);
        uint16_pool_ = std::make_unique<MemoryPool<uint16_t>>(initial_size);
        uint32_pool_ = std::make_unique<MemoryPool<uint32_t>>(initial_size);
        float_pool_ = std::make_unique<MemoryPool<float>>(initial_size);
        double_pool_ = std::make_unique<MemoryPool<double>>(initial_size);
        Logger::info("Common memory pools initialized with size " + std::to_string(initial_size));
    }

    static void shutdown()
    {
        std::lock_guard<std::mutex> lock(pools_mutex_);
        uint8_pool_.reset();
        uint16_pool_.reset();
        uint32_pool_.reset();
        float_pool_.reset();
        double_pool_.reset();
        Logger::info("Common memory pools shutdown");
    }

    static MemoryPool<uint8_t> *getUint8Pool() { return uint8_pool_.get(); }
    static MemoryPool<uint16_t> *getUint16Pool() { return uint16_pool_.get(); }
    static MemoryPool<uint32_t> *getUint32Pool() { return uint32_pool_.get(); }
    static MemoryPool<float> *getFloatPool() { return float_pool_.get(); }
    static MemoryPool<double> *getDoublePool() { return double_pool_.get(); }

    static void resetAll()
    {
        std::lock_guard<std::mutex> lock(pools_mutex_);
        if (uint8_pool_)
            uint8_pool_->reset();
        if (uint16_pool_)
            uint16_pool_->reset();
        if (uint32_pool_)
            uint32_pool_->reset();
        if (float_pool_)
            float_pool_->reset();
        if (double_pool_)
            double_pool_->reset();
        Logger::debug("All common memory pools reset");
    }

    static void printStatus()
    {
        std::lock_guard<std::mutex> lock(pools_mutex_);
        Logger::info("Memory Pool Status:");
        if (uint8_pool_)
        {
            Logger::info("  uint8: " + std::to_string(uint8_pool_->getTotalAllocated()) +
                         " allocated, " + std::to_string(uint8_pool_->getPoolCount()) + " pools");
        }
        if (uint16_pool_)
        {
            Logger::info("  uint16: " + std::to_string(uint16_pool_->getTotalAllocated()) +
                         " allocated, " + std::to_string(uint16_pool_->getPoolCount()) + " pools");
        }
        if (uint32_pool_)
        {
            Logger::info("  uint32: " + std::to_string(uint32_pool_->getTotalAllocated()) +
                         " allocated, " + std::to_string(uint32_pool_->getPoolCount()) + " pools");
        }
        if (float_pool_)
        {
            Logger::info("  float: " + std::to_string(float_pool_->getTotalAllocated()) +
                         " allocated, " + std::to_string(float_pool_->getPoolCount()) + " pools");
        }
        if (double_pool_)
        {
            Logger::info("  double: " + std::to_string(double_pool_->getTotalAllocated()) +
                         " allocated, " + std::to_string(double_pool_->getPoolCount()) + " pools");
        }
    }
};

// RAII wrapper for memory pool allocations
template <typename T>
class PoolAllocatedPtr
{
private:
    T *ptr_;
    MemoryPool<T> *pool_;
    size_t count_;

public:
    PoolAllocatedPtr(MemoryPool<T> *pool, size_t count)
        : pool_(pool), count_(count)
    {
        ptr_ = pool_ ? pool_->allocate(count) : new T[count];
    }

    ~PoolAllocatedPtr()
    {
        if (pool_)
        {
            // Memory will be reused by the pool
        }
        else
        {
            delete[] ptr_;
        }
    }

    T *get() { return ptr_; }
    const T *get() const { return ptr_; }
    T &operator[](size_t index) { return ptr_[index]; }
    const T &operator[](size_t index) const { return ptr_[index]; }

    // Disable copy
    PoolAllocatedPtr(const PoolAllocatedPtr &) = delete;
    PoolAllocatedPtr &operator=(const PoolAllocatedPtr &) = delete;

    // Allow move
    PoolAllocatedPtr(PoolAllocatedPtr &&other) noexcept
        : ptr_(other.ptr_), pool_(other.pool_), count_(other.count_)
    {
        other.ptr_ = nullptr;
        other.pool_ = nullptr;
        other.count_ = 0;
    }
};
