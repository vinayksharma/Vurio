## Configuration Rules and Playbook

This document defines the rules and the exact steps to add a new configuration property to the Unified Observable Configuration System. It is written for both humans and generative AI agents.

The system is centered around `UnifiedObservableConfigManager` which provides:

- Type-safe properties with `std::any` under the hood
- YAML file persistence and auto-reload
- Event-driven change notifications
- Pluggable and built-in validation

Key components (see `src/config` and `include/config`):

- `UnifiedObservableConfigManager` orchestrates everything
- `ConfigProperty` and `ObservableConfigProperty` represent properties
- `ConfigPropertyManager` stores properties
- `ConfigFileManager` handles YAML I/O
- `ConfigFileMonitor` watches for file changes (auto-reload)
- `ConfigEventManager` emits `ConfigChangeEvent`
- `ConfigValidator` validates properties and full configurations
- `ConfigTypeConverter` converts between `std::any`/native types/strings
- `ConfigManagerFactory` creates default properties and recommended wiring

### Quick checklist (TL;DR)

- Define a clear key name using dot-separated namespaces (e.g., `server.port`, `files.manager.scan.intervalMs`).
- Choose a concrete C++ type supported by the system.
- Add the property to `ConfigManagerFactory` with a default and description.
- If the subsystem needs live reaction, subscribe to config change events and handle the key.
- Add validation: built-in if covered; otherwise register a custom validator for the key.
- Update `config/config.yaml` if you want a checked-in default override; ensure it matches the factory default or intentionally differs.
- Document the new key in `config/CONFIGURATION_REFERENCE.md`.
- Add unit tests under `tests/` to cover default, parsing, validation, and event reaction.

### Supported property types

The system supports standard scalar types and a few collections. Commonly used:

- string-like: `std::string`
- integers: `int`, `long`, `long long`, `unsigned int`, `unsigned long`, `unsigned long long`
- floating-point: `float`, `double` (stored and stringified with fixed 6 decimal precision)
- boolean: `bool`
- collections: `std::vector<std::string>`

All values are stored as `std::any` inside properties. Use `ConfigTypeConverter` for safe conversions when needed.

### Step-by-step: Adding a new property

1. Pick the key and type

- Use a dot-separated namespace that fits existing conventions.
- Examples: `filesservice.scan.recursive`, `scheduler.jobs.fileScan.intervalMs`, `logging.level`.

2. Define defaults in the factory
   Add the property to `ConfigManagerFactory` (preferred) so the whole system sees a consistent default and description.

Example (add to `src/config/config_manager_factory.cpp` in the appropriate section):

```cpp
// Example: add a throttle toggle for file scans
manager->createProperty<bool>(
    "files.manager.throttle.enabled",
    false,
    "Enable throttling for files manager scans");

manager->createProperty<int>(
    "files.manager.throttle.maxFilesPerRun",
    10000,
    "Max files to process per run when throttling is enabled");
```

3. (Optional) Include in checked-in config file
   If you want the default visible/editable in the repo config, add it to `config/config.yaml`. Keep it in sync with the factory default unless there's a deliberate difference.

4. React to changes (event-driven)
   Subscribe to config changes and handle your key to apply live updates. Use the manager’s event API:

```cpp
configManager->subscribeToConfigChanges([
    /* capture subsystems or weak refs as needed */
](const MediaDedup::ConfigChangeEvent &evt) {
    if (evt.key == "files.manager.throttle.enabled") {
        // evt.new_value is std::any; convert safely
        bool enabled = false;
        try { enabled = std::any_cast<bool>(evt.new_value); } catch (...) {}
        myFilesManager->setThrottleEnabled(enabled);
    }
});
```

Notes:

- Event fields: `key`, `old_value`, `new_value`, `source` ("programmatic" | "file" | "default"), `is_file_update`.
- Keep callbacks lightweight; they run within the event emission loop.
- For bulk updates, consider batching your reactions.

5. Add validation
   Use built-in validation when your key falls under existing domains (server/logging/database/file scanner). Otherwise, register a custom validator:

```cpp
configManager->registerValidationCallback(
    "files.manager.throttle.maxFilesPerRun",
    [](const std::string &key, const std::any &value) -> bool {
        try {
            int v = std::any_cast<int>(value);
            return v > 0 && v <= 2'000'000; // reasonable bounds
        } catch (...) {
            return false;
        }
    }
);
```

You can enable/disable validation globally:

```cpp
configManager->setValidationEnabled(true);
```

6. Persisting and loading

- Programmatic changes: `configManager->setPropertyValue<T>(key, value);`
- File-originated changes: edit YAML; the monitor auto-reloads if enabled.
- Explicit persistence: `configManager->triggerSave();` (usually not required for single changes).

7. Auto-reload (reactive from file changes)

- File monitoring is on by default if the manager was constructed with monitoring enabled.
- Control at runtime:

```cpp
configManager->setAutoReload(true); // or false
configManager->setReloadInterval(std::chrono::milliseconds(1000));
```

On file change, the manager calls `reloadConfiguration()` and emits config change events.

### Coding patterns and APIs to use

Creating or accessing properties:

```cpp
// Create with default
auto prop = configManager->createProperty<int>("app.cache.maxEntries", 1000, "Cache size limit");

// Read with fallback
int maxEntries = configManager->getPropertyValue<int>("app.cache.maxEntries", 1000);

// Update (emits event)
configManager->setPropertyValue<int>("app.cache.maxEntries", 500);
```

Event subscription:

```cpp
configManager->subscribeToConfigChanges(
    [](const MediaDedup::ConfigChangeEvent &evt) {
        if (evt.key == "logging.level") {
            // convert std::any to std::string safely
            std::string level;
            try { level = std::any_cast<std::string>(evt.new_value); } catch (...) { return; }
            // apply to logger
            applyLoggingLevel(level);
        }
    }
);
```

Property-level conversion helpers (when you already have a `ConfigProperty`):

```cpp
auto p = configManager->getProperty<std::any>("app.cache.maxEntries");
if (p) {
    int v = 0;
    try { v = std::any_cast<int>(p->getValue()); } catch (...) {}
}
```

### Naming and structure conventions

- Use lowercase namespaces separated by dots: `domain.subdomain.name`.
- Prefer descriptive keys over abbreviations.
- Align with existing domains when possible: `server.*`, `logging.*`, `database.*`, `scheduler.*`, `filesservice.*`, `files.manager.*`, `tpm.*`.
- For per-job scheduler overrides, prefer `scheduler.jobs.<jobId>.<setting>`.

### Validation conventions

- Favor narrow, clear ranges and enums.
- Leverage existing built-in validators by placing keys under recognized domains.
- Use `registerValidationCallback` for bespoke constraints.
- Keep validation pure (no side-effects). Return `true` for valid, `false` for invalid; detailed errors are tracked internally by `ConfigValidator`.

### Reactivity policies (recommended)

- For components needing live updates, subscribe to change events and apply changes immediately.
- If a component polls configuration periodically, ensure it reads via `getPropertyValue<T>` each cycle.
- Avoid long/blocking work inside change callbacks; dispatch to background threads if necessary.
- For frequently updated properties, consider idempotent handlers and deduplicate if necessary.

### YAML and persistence

- YAML is the source of truth on disk; programmatic changes are persisted via `saveConfiguration()`.
- Keys not present in YAML will be created on first save with their current values.
- Unknown types fall back to string storage; prefer explicit typed defaults in the factory.

### Testing guidance

- Add unit tests that:
  - Verify default values from the factory
  - Parse YAML with valid and invalid values
  - Confirm validation accepts good values and rejects bad values
  - Assert change events fire with correct `old_value`, `new_value`, `source`, and `is_file_update`
  - Exercise auto-reload by touching the YAML file

### Anti-patterns (avoid)

- Creating properties ad-hoc in scattered code; use the factory.
- Using raw `std::any_cast` without try/catch in event handlers.
- Doing heavy work or blocking I/O directly inside change callbacks.
- Adding new keys to YAML without adding them to the factory and documentation.

### Reference: Relevant APIs (signatures)

- Property creation/access: see `include/config/unified_observable_config.hpp` and `include/config/config_property_manager.hpp`.
- Events: `subscribeToConfigChanges`, `emitConfigChangeEvent`, `ConfigChangeEvent`.
- Validation: `ConfigValidator::registerValidationCallback`, `setValidationEnabled`.
- Persistence: `saveConfiguration`, `triggerSave`, `setAutoReload`, `setReloadInterval`.

### Example end-to-end addition

1. Factory defaults:

```cpp
manager->createProperty<std::string>(
    "app.theme",
    "system",
    "UI theme: light|dark|system");
```

2. Custom validation:

```cpp
configManager->registerValidationCallback("app.theme",
    [](const std::string&, const std::any &v){
        try { auto s = std::any_cast<std::string>(v);
              return s == "light" || s == "dark" || s == "system"; }
        catch (...) { return false; }
    });
```

3. React to changes:

```cpp
configManager->subscribeToConfigChanges([](const ConfigChangeEvent &evt){
    if (evt.key == "app.theme") applyThemeSafely(evt);
});
```

4. Document in `CONFIGURATION_REFERENCE.md` and optionally add to `config.yaml`.

---

By following this playbook, new properties will be consistent, validated, reactive, and well-documented across the system.
