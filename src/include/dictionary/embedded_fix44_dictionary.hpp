#pragma once

#include <string>

// Embedded FIX 4.4 dictionary
// This allows the extension to work without requiring external dictionary files
// The actual data is generated at build time from the XML and stored as a byte array
// to avoid MSVC's 16KB string literal limitation.
namespace duckdb {

// Returns the embedded FIX 4.4 dictionary XML as a string
// The implementation is in src/dictionary/embedded_fix44_dictionary.cpp
// which is auto-generated at build time
std::string GetEmbeddedFix44Dictionary();

} // namespace duckdb
