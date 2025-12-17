#pragma once

#include "fix_message.hpp"
#include <cstdint>

// FIX message tokenizer
// Fast, zero-copy parsing of SOH-delimited FIX messages
class FixTokenizer {
public:
	// Parse a FIX message from a buffer
	// Supports both SOH ('\x01') and pipe ('|') delimiters
	// Returns true on success, false on parse error (error stored in msg)
	static bool Parse(const char *input, size_t input_len, ParsedFixMessage &msg, char delimiter = '\x01');

private:
	// Parse a single tag=value pair
	static bool ParseTag(const char *tag_str, size_t tag_len, const char *value, size_t value_len,
	                     ParsedFixMessage &msg);

	// Extract tag number from string
	static bool ExtractTagNumber(const char *tag_str, size_t tag_len, int &tag_out);

	// Helper: check if string is numeric
	static bool IsNumeric(const char *str, size_t len);
};
