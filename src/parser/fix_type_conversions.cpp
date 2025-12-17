#include "fix_type_conversions.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/time.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/common/string_util.hpp"
#include <stdexcept>
#include <cstring>

namespace duckdb {

void SetStringField(Vector &column, idx_t row, const char* ptr, size_t len) {
    if (ptr != nullptr && len > 0) {
        column.SetValue(row, Value(std::string(ptr, len)));
    } else {
        column.SetValue(row, Value());
    }
}

bool ConvertToInt64(const char* ptr, size_t len, int64_t &result, 
                    std::vector<std::string> &errors, const char* field_name) {
    if (ptr == nullptr || len == 0) {
        return false;
    }
    
    try {
        std::string str_val(ptr, len);
        size_t pos;
        result = std::stoll(str_val, &pos);
        
        if (pos != str_val.length()) {
            throw std::invalid_argument("extra characters");
        }
        
        return true;
    } catch (const std::exception& e) {
        errors.push_back(std::string("Invalid ") + field_name + ": '" + 
                        std::string(ptr, len) + "'");
        return false;
    }
}

bool ConvertToDouble(const char* ptr, size_t len, double &result,
                     std::vector<std::string> &errors, const char* field_name) {
    if (ptr == nullptr || len == 0) {
        return false;
    }
    
    try {
        std::string str_val(ptr, len);
        size_t pos;
        result = std::stod(str_val, &pos);
        
        if (pos != str_val.length()) {
            throw std::invalid_argument("extra characters");
        }
        
        return true;
    } catch (const std::exception& e) {
        errors.push_back(std::string("Invalid ") + field_name + ": '" + 
                        std::string(ptr, len) + "'");
        return false;
    }
}

bool ConvertToTimestamp(const char* ptr, size_t len, timestamp_t &result,
                        std::vector<std::string> &errors, const char* field_name) {
    if (ptr == nullptr || len < 17) {  // Minimum: YYYYMMDD-HH:MM:SS
        return false;
    }
    
    try {
        // Helper to parse 2 digits with bounds checking
        auto parse_2digits = [&](size_t offset) -> int {
            if (offset + 1 >= len) {
                throw std::runtime_error("Buffer overrun");
            }
            const char* p = ptr + offset;
            if (p[0] < '0' || p[0] > '9' || p[1] < '0' || p[1] > '9') {
                throw std::runtime_error("Invalid digit");
            }
            return (p[0] - '0') * 10 + (p[1] - '0');
        };
        
        // Helper to parse 4 digits with bounds checking
        auto parse_4digits = [&](size_t offset) -> int {
            if (offset + 3 >= len) {
                throw std::runtime_error("Buffer overrun");
            }
            const char* p = ptr + offset;
            for (int i = 0; i < 4; i++) {
                if (p[i] < '0' || p[i] > '9') {
                    throw std::runtime_error("Invalid digit");
                }
            }
            return (p[0] - '0') * 1000 + (p[1] - '0') * 100 + 
                   (p[2] - '0') * 10 + (p[3] - '0');
        };
        
        // Parse YYYYMMDD-HH:MM:SS[.sss]
        int year = parse_4digits(0);      // YYYY
        int month = parse_2digits(4);     // MM
        int day = parse_2digits(6);       // DD
        
        // Validate date components
        if (year < 1900 || year > 2100) {
            throw std::runtime_error("Year out of range");
        }
        if (month < 1 || month > 12) {
            throw std::runtime_error("Month out of range");
        }
        if (day < 1 || day > 31) {
            throw std::runtime_error("Day out of range");
        }
        
        // Check for separator at position 8
        if (ptr[8] != '-') {
            throw std::runtime_error("Missing date-time separator");
        }
        
        int hour = parse_2digits(9);      // HH
        int minute = parse_2digits(12);   // MM
        int second = parse_2digits(15);   // SS
        
        // Validate time components
        if (hour > 23) {
            throw std::runtime_error("Hour out of range");
        }
        if (minute > 59) {
            throw std::runtime_error("Minute out of range");
        }
        if (second > 59) {
            throw std::runtime_error("Second out of range");
        }
        
        // Check for time separators
        if (ptr[11] != ':' || ptr[14] != ':') {
            throw std::runtime_error("Missing time separators");
        }
        
        // Parse milliseconds if present and convert to microseconds
        int micros = 0;
        if (len > 17 && ptr[17] == '.') {
            // Parse up to 3 digits of milliseconds
            int ms = 0;
            int digits = 0;
            for (size_t i = 18; i < len && i < 21 && ptr[i] >= '0' && ptr[i] <= '9'; i++) {
                ms = ms * 10 + (ptr[i] - '0');
                digits++;
            }
            // Convert to microseconds (pad with zeros if needed)
            while (digits < 3) {
                ms *= 10;
                digits++;
            }
            micros = ms * 1000;  // milliseconds to microseconds
        }
        
        // Create timestamp (assuming UTC)
        date_t date = Date::FromDate(year, month, day);
        dtime_t time = Time::FromTime(hour, minute, second, micros);
        result = Timestamp::FromDatetime(date, time);
        
        return true;
    } catch (const std::exception& e) {
        errors.push_back(std::string("Invalid ") + field_name + ": '" + 
                        std::string(ptr, len) + "' (" + e.what() + ")");
        return false;
    }
}

} // namespace duckdb
