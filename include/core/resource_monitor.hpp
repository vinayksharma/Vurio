#pragma once
#include <atomic>
#include <map>
#include <mutex>
#include <chrono>
#include <string>
#include <memory>
#include <functional>
#include <iomanip>
#include <sstream>
#include "logging/logger.hpp"

class ResourceMonitor
{
public:
    // Resource usage statistics
    struct ResourceStats
    {
        std::atomic<size_t> current_usage{0};
        std::atomic<size_t> peak_usage{0};
        std::atomic<size_t> total_allocations{0};
        std::atomic<size_t> total_deallocations{0};
        std::atomic<size_t> allocation_count{0};
        std::atomic<size_t> deallocation_count{0};

        void recordAllocation(size_t size)
        {
            current_usage.fetch_add(size);
            total_allocations.fetch_add(size);
            allocation_count.fetch_add(1);

            size_t current = current_usage.load();
            size_t peak = peak_usage.load();
            while (current > peak && !peak_usage.compare_exchange_weak(peak, current))
            {
                // Retry until we successfully update the peak
            }
        }

        void recordDeallocation(size_t size)
        {
            current_usage.fetch_sub(size);
            total_deallocations.fetch_add(size);
            deallocation_count.fetch_add(1);
        }

        size_t getCurrentUsage() const { return current_usage.load(); }
        size_t getPeakUsage() const { return peak_usage.load(); }
        size_t getTotalAllocations() const { return total_allocations.load(); }
        size_t getTotalDeallocations() const { return total_deallocations.load(); }
        size_t getAllocationCount() const { return allocation_count.load(); }
        size_t getDeallocationCount() const { return deallocation_count.load(); }

        void reset()
        {
            current_usage.store(0);
            peak_usage.store(0);
            total_allocations.store(0);
            total_deallocations.store(0);
            allocation_count.store(0);
            deallocation_count.store(0);
        }

        // Copy constructor
        ResourceStats(const ResourceStats &other)
        {
            current_usage.store(other.current_usage.load());
            peak_usage.store(other.peak_usage.load());
            total_allocations.store(other.total_allocations.load());
            total_deallocations.store(other.total_deallocations.load());
            allocation_count.store(other.allocation_count.load());
            deallocation_count.store(other.deallocation_count.load());
        }

        // Assignment operator
        ResourceStats &operator=(const ResourceStats &other)
        {
            if (this != &other)
            {
                current_usage.store(other.current_usage.load());
                peak_usage.store(other.peak_usage.load());
                total_allocations.store(other.total_allocations.load());
                total_deallocations.store(other.total_deallocations.load());
                allocation_count.store(other.allocation_count.load());
                deallocation_count.store(other.deallocation_count.load());
            }
            return *this;
        }

        // Default constructor
        ResourceStats() = default;
    };

    // Memory leak detection settings
    struct LeakDetectionSettings
    {
        size_t warning_threshold_mb;
        size_t critical_threshold_mb;
        size_t leak_suspicion_threshold;
        bool enable_warnings;
        bool enable_critical_alerts;
        bool enable_leak_suspicion;

        LeakDetectionSettings()
            : warning_threshold_mb(100),      // 100MB
              critical_threshold_mb(500),     // 500MB
              leak_suspicion_threshold(1000), // 1000 allocations without deallocations
              enable_warnings(true),
              enable_critical_alerts(true),
              enable_leak_suspicion(true)
        {
        }
    };

    // Initialize resource monitor
    static void initialize(const LeakDetectionSettings &settings = LeakDetectionSettings{})
    {
        std::lock_guard<std::mutex> lock(monitor_mutex_);
        if (!instance_)
        {
            instance_ = std::make_unique<ResourceMonitor>(settings);
            Logger::info("Resource monitor initialized");
        }
    }

    // Shutdown resource monitor
    static void shutdown()
    {
        std::lock_guard<std::mutex> lock(monitor_mutex_);
        if (instance_)
        {
            instance_->printFinalReport();
            instance_.reset();
            Logger::info("Resource monitor shutdown");
        }
    }

    // Get singleton instance
    static ResourceMonitor &getInstance()
    {
        if (!instance_)
        {
            throw std::runtime_error("ResourceMonitor not initialized. Call initialize() first.");
        }
        return *instance_;
    }

    // Record memory allocation
    void recordAllocation(size_t size, const std::string &category = "unknown",
                          const std::string &operation = "unknown")
    {
        std::lock_guard<std::mutex> lock(stats_mutex_);

        // Update global stats
        global_stats_.recordAllocation(size);

        // Update category stats
        category_stats_[category].recordAllocation(size);

        // Update operation stats
        operation_stats_[operation].recordAllocation(size);

        // Check thresholds
        checkThresholds(category, operation);

        // Log if suspicious activity detected
        if (settings_.enable_leak_suspicion)
        {
            checkLeakSuspicion(category, operation);
        }
    }

    // Record memory deallocation
    void recordDeallocation(size_t size, const std::string &category = "unknown",
                            const std::string &operation = "unknown")
    {
        std::lock_guard<std::mutex> lock(stats_mutex_);

        // Update global stats
        global_stats_.recordDeallocation(size);

        // Update category stats
        category_stats_[category].recordDeallocation(size);

        // Update operation stats
        operation_stats_[operation].recordDeallocation(size);
    }

    // Get resource statistics
    ResourceStats getGlobalStats() const
    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        return global_stats_;
    }

    ResourceStats getCategoryStats(const std::string &category) const
    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        auto it = category_stats_.find(category);
        return it != category_stats_.end() ? it->second : ResourceStats{};
    }

    ResourceStats getOperationStats(const std::string &operation) const
    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        auto it = operation_stats_.find(operation);
        return it != operation_stats_.end() ? it->second : ResourceStats{};
    }

    // Print current resource report
    void printResourceReport() const
    {
        std::lock_guard<std::mutex> lock(stats_mutex_);

        Logger::info("=== Resource Monitor Report ===");
        Logger::info("Global Memory Usage:");
        Logger::info("  Current: " + formatBytes(global_stats_.getCurrentUsage()));
        Logger::info("  Peak: " + formatBytes(global_stats_.getPeakUsage()));
        Logger::info("  Total Allocated: " + formatBytes(global_stats_.getTotalAllocations()));
        Logger::info("  Total Deallocated: " + formatBytes(global_stats_.getTotalDeallocations()));
        Logger::info("  Allocation Count: " + std::to_string(global_stats_.getAllocationCount()));
        Logger::info("  Deallocation Count: " + std::to_string(global_stats_.getDeallocationCount()));

        Logger::info("Category Breakdown:");
        for (const auto &[category, stats] : category_stats_)
        {
            Logger::info("  " + category + ": " + formatBytes(stats.getCurrentUsage()) +
                         " (peak: " + formatBytes(stats.getPeakUsage()) + ")");
        }

        Logger::info("Operation Breakdown:");
        for (const auto &[operation, stats] : operation_stats_)
        {
            Logger::info("  " + operation + ": " + formatBytes(stats.getCurrentUsage()) +
                         " (peak: " + formatBytes(stats.getPeakUsage()) + ")");
        }
        Logger::info("================================");
    }

    // Reset all statistics
    void resetAllStats()
    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        global_stats_.reset();
        for (auto &[category, stats] : category_stats_)
        {
            stats.reset();
        }
        for (auto &[operation, stats] : operation_stats_)
        {
            stats.reset();
        }
        Logger::info("All resource statistics reset");
    }

    // Set custom threshold checker
    void setThresholdChecker(std::function<void(const std::string &, const std::string &, size_t)> checker)
    {
        custom_threshold_checker_ = checker;
    }

    // Enable/disable monitoring
    void setMonitoringEnabled(bool enabled)
    {
        monitoring_enabled_.store(enabled);
        Logger::info("Resource monitoring " + std::string(enabled ? "enabled" : "disabled"));
    }

    bool isMonitoringEnabled() const
    {
        return monitoring_enabled_.load();
    }

    // Constructor for singleton pattern
    ResourceMonitor(const LeakDetectionSettings &settings)
        : settings_(settings), monitoring_enabled_(true) {}

private:
    static std::unique_ptr<ResourceMonitor> instance_;
    static std::mutex monitor_mutex_;

    LeakDetectionSettings settings_;
    std::atomic<bool> monitoring_enabled_;

    mutable std::mutex stats_mutex_;
    ResourceStats global_stats_;
    std::map<std::string, ResourceStats> category_stats_;
    std::map<std::string, ResourceStats> operation_stats_;

    std::function<void(const std::string &, const std::string &, size_t)> custom_threshold_checker_;

    // Check memory thresholds
    void checkThresholds(const std::string &category, const std::string &operation)
    {
        size_t current_mb = global_stats_.getCurrentUsage() / (1024 * 1024);

        if (settings_.enable_warnings && current_mb > settings_.warning_threshold_mb)
        {
            Logger::warn("Memory usage warning: " + std::to_string(current_mb) +
                         "MB (threshold: " + std::to_string(settings_.warning_threshold_mb) + "MB)");
        }

        if (settings_.enable_critical_alerts && current_mb > settings_.critical_threshold_mb)
        {
            Logger::error("CRITICAL: Memory usage exceeded critical threshold: " +
                          std::to_string(current_mb) + "MB (threshold: " +
                          std::to_string(settings_.critical_threshold_mb) + "MB)");
        }

        if (custom_threshold_checker_)
        {
            custom_threshold_checker_(category, operation, global_stats_.getCurrentUsage());
        }
    }

    // Check for potential memory leaks
    void checkLeakSuspicion(const std::string &category, const std::string &operation)
    {
        size_t alloc_count = global_stats_.getAllocationCount();
        size_t dealloc_count = global_stats_.getDeallocationCount();

        if (alloc_count - dealloc_count > settings_.leak_suspicion_threshold)
        {
            Logger::warn("Potential memory leak detected: " +
                         std::to_string(alloc_count - dealloc_count) +
                         " more allocations than deallocations");
        }
    }

    // Print final report before shutdown
    void printFinalReport() const
    {
        Logger::info("=== Final Resource Report ===");
        printResourceReport();

        // Check for unreleased resources
        size_t unreleased = global_stats_.getCurrentUsage();
        if (unreleased > 0)
        {
            Logger::warn("Unreleased memory at shutdown: " + formatBytes(unreleased));
        }
    }

    // Format bytes to human-readable string
    static std::string formatBytes(size_t bytes)
    {
        const char *units[] = {"B", "KB", "MB", "GB", "TB"};
        int unit_index = 0;
        double size = static_cast<double>(bytes);

        while (size >= 1024.0 && unit_index < 4)
        {
            size /= 1024.0;
            unit_index++;
        }

        std::stringstream ss;
        ss << std::fixed << std::setprecision(2) << size << " " << units[unit_index];
        return ss.str();
    }
};

// Static member declarations (defined in .cpp file)

// RAII wrapper for automatic resource monitoring
class ScopedResourceMonitor
{
private:
    std::string category_;
    std::string operation_;
    size_t allocation_size_;
    bool deallocated_;

public:
    ScopedResourceMonitor(size_t size, const std::string &category, const std::string &operation)
        : category_(category), operation_(operation), allocation_size_(size), deallocated_(false)
    {
        if (ResourceMonitor::getInstance().isMonitoringEnabled())
        {
            ResourceMonitor::getInstance().recordAllocation(size, category, operation);
        }
    }

    ~ScopedResourceMonitor()
    {
        if (!deallocated_ && ResourceMonitor::getInstance().isMonitoringEnabled())
        {
            ResourceMonitor::getInstance().recordDeallocation(allocation_size_, category_, operation_);
        }
    }

    // Mark as manually deallocated
    void markDeallocated()
    {
        if (!deallocated_ && ResourceMonitor::getInstance().isMonitoringEnabled())
        {
            ResourceMonitor::getInstance().recordDeallocation(allocation_size_, category_, operation_);
            deallocated_ = true;
        }
    }

    // Disable copy
    ScopedResourceMonitor(const ScopedResourceMonitor &) = delete;
    ScopedResourceMonitor &operator=(const ScopedResourceMonitor &) = delete;
};
