#include "core/memory_pool.hpp"
#include "core/resource_monitor.hpp"

// Initialize static members for CommonMemoryPools
std::unique_ptr<MemoryPool<uint8_t>> CommonMemoryPools::uint8_pool_;
std::unique_ptr<MemoryPool<uint16_t>> CommonMemoryPools::uint16_pool_;
std::unique_ptr<MemoryPool<uint32_t>> CommonMemoryPools::uint32_pool_;
std::unique_ptr<MemoryPool<float>> CommonMemoryPools::float_pool_;
std::unique_ptr<MemoryPool<double>> CommonMemoryPools::double_pool_;
std::mutex CommonMemoryPools::pools_mutex_;

// Initialize static members for ResourceMonitor
std::unique_ptr<ResourceMonitor> ResourceMonitor::instance_;
std::mutex ResourceMonitor::monitor_mutex_;
