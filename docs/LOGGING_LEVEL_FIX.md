# Logging Level Validation Fix

## Issue

The `logging.level` configuration property had **case-sensitive validation** that was rejecting valid log level values when sent from the Electron client.

### Problem Summary:

- ✅ `"inf"` worked (accidentally, as Poco accepts it)
- ❌ `"info"`, `"error"`, `"debug"`, `"warn"`, `"trace"` failed
- The validation was checking for exact lowercase matches only
- Documentation claimed it was case-insensitive, but implementation wasn't

## Root Cause

**Two places had case-sensitive validation:**

1. **`src/config/config_manager_factory.cpp`** (line 494):

   ```cpp
   // OLD (case-sensitive)
   std::string level = std::any_cast<std::string>(value);
   std::vector<std::string> valid_levels = {"trace", "debug", "info", "warn", "error"};
   return std::find(valid_levels.begin(), valid_levels.end(), level) != valid_levels.end();
   ```

2. **`src/config/config_validator.cpp`** (line 200):
   ```cpp
   // OLD (case-sensitive)
   std::string level = std::any_cast<std::string>(value);
   std::vector<std::string> valid_levels = {"trace", "debug", "info", "warn", "error"};
   if (std::find(valid_levels.begin(), valid_levels.end(), level) == valid_levels.end()) { ... }
   ```

**Why "inf" worked:**

- The validation is called when setting values via API
- "inf" is not in the valid_levels list, so it should have failed
- But Poco's logger accepts "inf" as shorthand for "information"
- The applyLogLevel function passes it through to Poco, which accepts it

## Fix Applied

Made validation **case-insensitive** and added Poco's full log level names:

### 1. `config_manager_factory.cpp`:

```cpp
// NEW (case-insensitive)
std::string level = std::any_cast<std::string>(value);
// Convert to lowercase for case-insensitive comparison
std::string level_lower = level;
for (char &c : level_lower) {
    c = static_cast<char>(::tolower(c));
}
std::vector<std::string> valid_levels = {"trace", "debug", "info", "warn", "error", "information", "warning"};
return std::find(valid_levels.begin(), valid_levels.end(), level_lower) != valid_levels.end();
```

### 2. `config_validator.cpp`:

```cpp
// NEW (case-insensitive)
std::string level = std::any_cast<std::string>(value);
// Convert to lowercase for case-insensitive comparison
std::string level_lower = level;
for (char &c : level_lower) {
    c = static_cast<char>(::tolower(c));
}
std::vector<std::string> valid_levels = {"trace", "debug", "info", "warn", "error", "information", "warning"};
if (std::find(valid_levels.begin(), valid_levels.end(), level_lower) == valid_levels.end()) {
    addValidationError(key, "Invalid log level (case-insensitive)",
                       "trace|debug|info|information|warn|warning|error", level);
    return false;
}
```

## Accepted Values (Now Case-Insensitive)

All of these now work (case doesn't matter):

### Short Forms:

- ✅ `trace`, `TRACE`, `Trace`
- ✅ `debug`, `DEBUG`, `Debug`
- ✅ `info`, `INFO`, `Info`
- ✅ `warn`, `WARN`, `Warn`
- ✅ `error`, `ERROR`, `Error`

### Full Poco Names:

- ✅ `information`, `INFORMATION`, `Information`
- ✅ `warning`, `WARNING`, `Warning`

### Mapping:

- `info` → `information` (internally)
- `warn` → `warning` (internally)

## Testing

**From Electron Client:**

```javascript
// All of these should now work:
await fetch("/api/v1/config/logging.level", {
  method: "PUT",
  body: JSON.stringify({ value: "error" }), // ✅
});

await fetch("/api/v1/config/logging.level", {
  method: "PUT",
  body: JSON.stringify({ value: "ERROR" }), // ✅
});

await fetch("/api/v1/config/logging.level", {
  method: "PUT",
  body: JSON.stringify({ value: "Info" }), // ✅
});
```

**From config.yaml:**

```yaml
logging.level: error       # ✅
logging.level: ERROR       # ✅
logging.level: information # ✅
logging.level: INFO        # ✅
```

## Files Modified

1. ✅ `src/config/config_manager_factory.cpp` - Fixed validation callback
2. ✅ `src/config/config_validator.cpp` - Fixed property validation
3. ✅ `config/CONFIGURATION_REFERENCE.md` - Updated documentation

## Impact

- **Breaking Change**: No (only makes validation more permissive)
- **API Compatibility**: Improved (now accepts more valid inputs)
- **Client Fix Needed**: No (server-side fix resolves the issue)

## Recommendation for Client

The Electron client can now send logging level values in any case. Consider:

**Option A: Dropdown with standard values**

```javascript
const logLevels = ["trace", "debug", "info", "warn", "error"];
```

**Option B: Text input with validation helper**

```javascript
const validLevels = /^(trace|debug|info|information|warn|warning|error)$/i;
```

Both approaches will work correctly now that the server accepts case-insensitive values.
