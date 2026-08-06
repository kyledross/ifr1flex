# Contributing
## Development environment
This is a C++23 CMake project. Configure it with CMake 3.25 or later, a C++ compiler, Python 3, OpenGL development files, and the build tool selected by CMake. The Docker build is the supported reproducible Linux build path.

Use source-controlled CMake presets for shared build modes:

```bash
cmake --preset debug
cmake --build build/debug
ctest --test-dir build/debug --output-on-failure
```

Use `release` in place of `debug` for an optimized build. Put developer-specific CMake settings in `CMakeUserPresets.json`; it is not tracked.

Run the portable Docker build and tests with:

```bash
./docker-build.sh
```

## Project conventions
- Keep core logic in `src/core` testable through interfaces, including `IXPlaneSDK` for X-Plane SDK interactions.
- Use target-based CMake configuration rather than global include or link commands.
- Use Google Test and Google Mock for unit tests.
- Use C++ language features and C++ casts instead of C-style alternatives.
- Do not introduce reserved identifiers into the global namespace.
- Verify X-Plane dataref types with `IXPlaneSDK::GetDataRefTypes()` and match integer datarefs to `GetDatai`/`SetDatai` and floating-point datarefs to `GetDataf`/`SetDataf`.

## Configuration and settings
Aircraft mappings are JSON files in `configs`. Core control logic supports modes and shifted state, and evaluates LED behavior through range- and bit-mask tests.

`SettingsManager` owns plugin-wide settings in a `settings.json` file beside the plugin binary. Add defaults in `SettingsManager::SetDefaultSettings()` and retrieve boolean values with `SettingsManager::GetBool(name)`. The settings UI populates its entries dynamically from `SettingsManager`.

## Local artifacts
IDE state, agent-specific guidance, compilation databases, caches, and build/output directories are ignored. Do not add these generated files to version control.
