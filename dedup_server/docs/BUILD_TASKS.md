# Build Tasks and IDE Configuration

This document describes the available build tasks and IDE configuration for the Media Deduplication Server project.

## 🚀 Quick Start (VS Code / Cursor)

### Default Build Task (Cmd+Shift+B / Ctrl+Shift+B)

Press **Cmd+Shift+B** (macOS) or **Ctrl+Shift+B** (Linux/Windows) to run the **fast rebuild with all tests**.

This runs `./scripts/smart_rebuild` which:
- ✅ Cleans all build artifacts
- ✅ **Preserves dependency cache** (no re-download of ONNX Runtime)
- ✅ Full project rebuild
- ✅ Runs all unit tests
- ✅ Runs integration tests

**Time:** ~30-60 seconds (depends on changes)

## 📋 Available Build Tasks

All tasks can be accessed via:
- **Cmd+Shift+P** (macOS) or **Ctrl+Shift+P** (Linux/Windows)
- Type: "Tasks: Run Task"
- Select from the list below

### 1. Rebuild and Test All (Fast) ⭐ DEFAULT

**Command:** `./scripts/smart_rebuild`

**What it does:**
- Complete clean of build artifacts
- Preserves `build/_deps/` cache (ONNX Runtime stays cached)
- Full rebuild from scratch
- Runs all unit tests
- Runs integration tests via ctest

**When to use:**
- Regular development workflow
- Before committing changes
- After pulling updates
- Testing comprehensive changes

**Time:** ~30-60 seconds

---

### 2. Full Rebuild (Clean Everything)

**Command:** `./scripts/rebuild`

**What it does:**
- **Complete clean** including all dependencies
- **Re-downloads** ONNX Runtime (~200MB)
- Full rebuild from scratch
- Runs all unit tests
- Runs integration tests via ctest

**When to use:**
- Suspected dependency corruption
- Major CMake configuration changes
- Clean slate verification
- Troubleshooting build issues

**Time:** ~3-5 minutes (includes download)

---

### 3. Clean Build Server

**Command:** `./build.sh --clean`

**What it does:**
- Cleans build artifacts (preserves `_deps/`)
- Full rebuild
- **Does NOT run tests**

**When to use:**
- Quick rebuild without testing
- Just want to verify compilation
- Testing build system changes

**Time:** ~20-40 seconds

---

### 4. Build Server (No Clean)

**Command:** `./build.sh`

**What it does:**
- Incremental build (only changed files)
- **Does NOT run tests**

**When to use:**
- Rapid iteration during development
- Small changes to single files
- Quick compilation check

**Time:** ~5-15 seconds

---

### 5. Debug Build

**Command:** `./build.sh --clean --debug`

**What it does:**
- Cleans build artifacts
- Builds with debug symbols (`-g -O0`)
- Enables all warnings
- **Does NOT run tests**

**When to use:**
- Preparing for debugging session
- Need debug symbols for GDB/LLDB
- Investigating crashes or issues

**Time:** ~30-50 seconds

---

### 6. Build and Test

**Command:** `./build.sh --clean --test`

**What it does:**
- Cleans build artifacts (preserves `_deps/`)
- Full rebuild
- Runs unit tests only (not integration tests)

**When to use:**
- Quick test validation
- Don't need ctest integration tests
- Faster than smart_rebuild

**Time:** ~25-45 seconds

---

## 🎯 Recommended Workflow

### Daily Development
```bash
# Make changes to code
# Press Cmd+Shift+B (runs smart_rebuild)
# All tests pass → commit
```

### After Git Pull
```bash
# git pull origin main
# Press Cmd+Shift+B (runs smart_rebuild)
# Verify everything builds and tests pass
```

### Troubleshooting Build Issues
```bash
# Cmd+Shift+P → "Full Rebuild (Clean Everything)"
# Re-downloads all dependencies
# Clean slate build
```

### Quick Iteration (No Tests)
```bash
# Cmd+Shift+P → "Build Server (No Clean)"
# Fast incremental build
# Manually run: build/bin/media_dedup_server
```

---

## 🔧 Manual Build Scripts

You can also run build scripts directly from the terminal:

```bash
# Smart rebuild (fast, preserves cache)
./scripts/smart_rebuild

# Full rebuild (clean everything)
./scripts/rebuild

# Standard build
./build.sh

# Build with options
./build.sh --clean          # Clean build
./build.sh --debug          # Debug build
./build.sh --test           # Build and test
./build.sh --clean --test   # Clean, build, and test
```

---

## 📊 Build Task Comparison

| Task | Clean | Cache | Tests | Time | Use Case |
|------|-------|-------|-------|------|----------|
| **Smart Rebuild** ⭐ | ✅ | ✅ Keep | ✅ All | ~45s | **Default development** |
| **Full Rebuild** | ✅ | ❌ Delete | ✅ All | ~4m | Troubleshooting |
| **Clean Build** | ✅ | ✅ Keep | ❌ | ~30s | Quick compile check |
| **Incremental** | ❌ | ✅ Keep | ❌ | ~10s | Rapid iteration |
| **Debug Build** | ✅ | ✅ Keep | ❌ | ~40s | Debugging session |
| **Build & Test** | ✅ | ✅ Keep | ⚠️ Unit | ~35s | Quick validation |

---

## 🛠️ IDE Configuration Files

The following VS Code/Cursor configuration files are tracked in the repository:

### `.vscode/tasks.json`
Defines all build tasks and keyboard shortcuts.

### `.vscode/launch.json`
Debugger configuration for F5 debugging.

### `.vscode/settings.json`
Project-specific editor settings (formatting, linting, etc.).

### `.vscode/keybindings.json`
Custom keyboard shortcuts for build and debug operations.

---

## 🔑 Keyboard Shortcuts (VS Code / Cursor)

| Shortcut | Task | Description |
|----------|------|-------------|
| **Cmd+Shift+B** | Smart Rebuild | Fast rebuild with tests (default) |
| **F5** | Debug Build & Run | Build in debug mode and attach debugger |
| **Shift+F5** | Stop Debugger | Stop debugging session |
| **Cmd+Shift+P** | Command Palette | Access all tasks |

---

## 🐛 Debugging

### Quick Debug (F5)

1. Set breakpoints in your code
2. Press **F5**
3. Builds in debug mode automatically
4. Attaches debugger to the server
5. Server starts and breaks at your breakpoints

See `.vscode/launch.json` for debugger configuration.

---

## 📦 Dependency Cache

### What is cached?
- ONNX Runtime headers (~200MB tarball)
- External dependencies from CMake FetchContent
- Located in `build/_deps/`

### When is cache cleared?
- **Full Rebuild** task (deliberate)
- Manual `rm -rf build`
- CMake dependency version changes

### When is cache preserved?
- **Smart Rebuild** task ✅
- **Clean Build** task ✅
- All other build tasks ✅

---

## 💡 Tips

1. **Use Smart Rebuild for daily work** - It's fast and thorough
2. **Run Full Rebuild weekly** - Ensures clean dependency state
3. **Use Incremental builds** - For rapid testing during development
4. **Always test before committing** - Cmd+Shift+B runs all tests
5. **Debug with F5** - Automatic debug build and attach

---

## 🚨 Common Issues

### "Tests failed after rebuild"
- Check test output for specific failures
- Try "Full Rebuild (Clean Everything)" to rule out cache issues
- Verify database and config files are not corrupted

### "Build takes forever"
- Check if you're running Full Rebuild (re-downloads dependencies)
- Use Smart Rebuild instead for faster builds
- Verify network connection if downloads are slow

### "CMake configuration errors"
- Run Full Rebuild to refresh CMake cache
- Check that all dependencies are installed
- Review CMake error messages in the terminal

### "Can't find build tasks"
- Ensure you're in the project root directory
- Reload VS Code/Cursor window
- Check that `.vscode/tasks.json` exists

---

## 📚 Related Documentation

- [Build Scripts](../scripts/README.md) - Manual build script documentation
- [Testing Guide](tests_README.md) - Comprehensive testing documentation
- [Configuration Reference](CONFIGURATION_REFERENCE.md) - Server configuration
- [Development Setup](../README.md) - Initial setup and dependencies

---

**Last Updated:** October 2025

