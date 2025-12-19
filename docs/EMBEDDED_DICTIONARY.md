# Embedded FIX Dictionary

## Overview

The QuackFIX extension includes an embedded FIX 4.4 dictionary that is compiled directly into the binary. This eliminates the need for users to distribute separate XML dictionary files with the extension.

## Implementation

### Why Byte Array Instead of String Literal?

The FIX 4.4 dictionary XML is approximately 315KB. Microsoft Visual C++ (MSVC) has a 16KB limit on string literals, which would prevent the extension from compiling on Windows. To solve this, we use a **byte array** approach that works across all platforms:

- **Linux**: ✓ Compiles successfully
- **macOS**: ✓ Compiles successfully  
- **Windows (MSVC)**: ✓ Compiles successfully (no string literal limit)
- **WebAssembly**: ✓ Compiles successfully

### Build Process

1. **Source XML**: `data/fix44_dictionary.xml` contains the FIX 4.4 dictionary
2. **Generator Script**: `scripts/generate_embedded_dictionary.py` converts XML to C++ byte array
3. **Generated File**: `build/release/extension/quackfix/embedded_fix44_dictionary.cpp` (auto-generated at build time)
4. **Header File**: `src/include/dictionary/embedded_fix44_dictionary.hpp` declares the accessor function

### Architecture

```
data/fix44_dictionary.xml
    ↓ (CMake runs generator at build time)
scripts/generate_embedded_dictionary.py
    ↓ (generates)
build/*/embedded_fix44_dictionary.cpp
    ├─ static const unsigned char embedded_fix44_dict_data[] = { ... }
    ├─ static const size_t embedded_fix44_dict_size = 315399
    └─ std::string GetEmbeddedFix44Dictionary() { ... }
```

### Usage in Code

```cpp
#include "dictionary/embedded_fix44_dictionary.hpp"

// Get the embedded dictionary
std::string xml = duckdb::GetEmbeddedFix44Dictionary();

// Load it
FixDictionary dict = FixDictionaryLoader::LoadFromString(xml);
```

The `read_fix()` table function automatically uses the embedded dictionary when no `dictionary` parameter is specified.

## Modifying the Dictionary

### To update the embedded dictionary:

1. Edit `data/fix44_dictionary.xml`
2. Run `make clean && make`
3. The build system automatically regenerates the byte array

### To add a different FIX version:

1. Add new XML file (e.g., `data/fix50_dictionary.xml`)
2. Update `CMakeLists.txt` to generate additional byte array
3. Create new accessor function in header
4. Use the new function in your code

## Technical Details

### Generated Code Structure

The Python script generates optimized C++ code:

```cpp
static const unsigned char embedded_fix44_dict_data[] = {
    0x3c, 0x66, 0x69, 0x78, 0x20, 0x74, 0x79, 0x70, 0x65, 0x3d, 0x27, 0x46,
    // ... thousands more bytes (12 per line for readability)
};

static const size_t embedded_fix44_dict_size = 315399;

std::string GetEmbeddedFix44Dictionary() {
    return std::string(
        reinterpret_cast<const char*>(embedded_fix44_dict_data),
        embedded_fix44_dict_size
    );
}
```

### Performance

- **Memory**: ~315KB embedded in binary (minimal overhead)
- **Load Time**: Near-instant (no file I/O required)
- **Build Time**: +1-2 seconds for generation (only when XML changes)

## Cross-Platform Compatibility

This approach is **100% portable** and works on:

- **Linux** (GCC, Clang)
- **macOS** (Apple Clang)
- **Windows** (MSVC, MinGW)
- **WebAssembly** (Emscripten)

Unlike platform-specific resource embedding (`.rc` files, `ld -r -b binary`, etc.), byte arrays are standard C++ and work everywhere.

## Maintenance

The embedded dictionary is automatically regenerated whenever:

1. `data/fix44_dictionary.xml` is modified
2. `scripts/generate_embedded_dictionary.py` is modified
3. A clean build is performed

No manual intervention is required during normal development.
