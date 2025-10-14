# Shutdown Deadlock Fix

## Problem

The server would not respond to `Ctrl+C` (SIGINT) and appeared to be blocked during shutdown. Analysis using `sample` revealed a deadlock:

### Deadlock Scenario

1. **Main Thread**: Trying to shut down, calls `MediaProcessor::shutdown()` → `unsubscribeFromConfigChanges()` → Blocked waiting for `ConfigEventManager` mutex
2. **ConfigFileMonitor Thread**: Detected config file change, acquired `ConfigEventManager` mutex, called callbacks → `ThreadPoolManager::recreateThreadPool()` → Blocked waiting for thread pool to drain

This created a circular dependency where:

- Main thread needs the config mutex (held by ConfigFileMonitor)
- ConfigFileMonitor needs the thread pool to drain (blocked by shutdown process)

## Root Causes

### 1. ConfigEventManager Held Mutex During Callbacks

In `src/config/config_event_manager.cpp`, the `notifyConfigChange()` method held the `callbacks_mutex_` while iterating through and calling all callbacks. If any callback performed blocking operations (like waiting for threads to drain), it would cause a deadlock.

### 2. Incorrect Shutdown Order

The `ConfigFileMonitor` thread was never explicitly stopped during shutdown. It only stopped when the `UnifiedObservableConfigManager` destructor was called, which happened after all other components had been shut down. This meant the monitor could trigger config reloads during shutdown.

## Solution

### Fix 1: Release Mutex Before Calling Callbacks

**File**: `src/config/config_event_manager.cpp`

Modified `notifyConfigChange()` to:

1. Copy the callback list while holding the mutex
2. Release the mutex
3. Call the callbacks without holding the mutex

This prevents deadlock if callbacks perform blocking operations, as they no longer hold the config mutex.

```cpp
void ConfigEventManager::notifyConfigChange(const ConfigChangeEvent &event)
{
    // Copy callbacks while holding the lock
    std::vector<ConfigChangeCallback> callbacks_copy;
    {
        std::lock_guard<std::mutex> lock(callbacks_mutex_);
        callbacks_copy = config_change_callbacks_;
    }

    // Call callbacks outside the lock to prevent deadlock
    for (const auto &callback : callbacks_copy)
    {
        // ... call callback ...
    }
}
```

### Fix 2: Stop ConfigFileMonitor First During Shutdown

**File**: `src/core/server_server_shutdown.cpp`

Added an explicit step at the **beginning** of the shutdown sequence to stop the `ConfigFileMonitor`:

```cpp
void ServerShutdown::handleShutdown(...)
{
    // Stop config file monitoring FIRST
    if (config_manager_)
    {
        config_manager_->shutdown();  // Stops ConfigFileMonitor thread
    }

    // Then stop other components...
    // - Console input
    // - Scheduler
    // - MediaProcessor
    // - ThreadPoolManager
    // - WebServer
    // - Database
}
```

This ensures the `ConfigFileMonitor` thread is not running when we shut down components that it might interact with.

## Testing

Created a comprehensive test script (`/tmp/test_deadlock_fix.sh`) that:

1. Starts the server with stdin from a FIFO
2. Waits for full startup
3. Triggers a config file change (touches `config/config.yaml`)
4. Immediately sends SIGINT
5. Monitors shutdown time (10-second timeout)

**Result**: Server shut down cleanly in **2 seconds**, confirming the deadlock is fixed.

## Benefits

1. **Responsive Shutdown**: Server now responds to `Ctrl+C` immediately
2. **No Deadlock Risk**: Config callbacks can perform blocking operations safely
3. **Clean Shutdown Order**: Components shut down in the correct dependency order
4. **Thread Safety**: Mutex is released before potentially long-running callbacks

## Files Modified

- `src/config/config_event_manager.cpp` - Fixed mutex holding during callbacks
- `src/core/server_server_shutdown.cpp` - Added ConfigFileMonitor shutdown at start of sequence

## Related Issues

- Signal handling (`SIGINT`, `SIGTERM`) is now reliable
- No more hanging during graceful shutdown
- Config file changes during shutdown no longer cause issues
