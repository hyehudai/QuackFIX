#pragma once

#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/vector.hpp"
#include <string>
#include <vector>

namespace duckdb {

// Centralized type conversion helpers for FIX parsing
// All conversions are lenient and collect errors rather than throwing exceptions

// Set a string field from pointer/length pair
void SetStringField(Vector &column, idx_t row, const char* ptr, size_t len);

// Convert string to int64 with error collection
bool ConvertToInt64(const char* ptr, size_t len, int64_t &result, 
                    std::vector<std::string> &errors, const char* field_name);

// Convert string to double with error collection
bool ConvertToDouble(const char* ptr, size_t len, double &result,
                     std::vector<std::string> &errors, const char* field_name);

// Convert FIX timestamp string to DuckDB timestamp with error collection
// Format: YYYYMMDD-HH:MM:SS[.sss]
// Example: 20231215-10:30:00 or 20231215-10:30:00.123
bool ConvertToTimestamp(const char* ptr, size_t len, timestamp_t &result,
                        std::vector<std::string> &errors, const char* field_name);

} // namespace duckdb
