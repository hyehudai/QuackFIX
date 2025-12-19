#pragma once

#include <string>
#include <unordered_map>
#include <vector>
#include <utility>
#include <cstdint>
#include <cstring>

// Parsed FIX message structure
// Stores hot tags as direct fields and other tags in a map
// Uses empty strings to indicate absence (length == 0)
struct ParsedFixMessage {
	// Hot tags (always parsed)
	// Empty string_view (data() == nullptr or size() == 0) means not set
	const char *msg_type; // Tag 35
	size_t msg_type_len;
	const char *sender_comp_id; // Tag 49
	size_t sender_comp_id_len;
	const char *target_comp_id; // Tag 56
	size_t target_comp_id_len;
	const char *msg_seq_num; // Tag 34
	size_t msg_seq_num_len;
	const char *sending_time; // Tag 52
	size_t sending_time_len;
	const char *cl_ord_id; // Tag 11
	size_t cl_ord_id_len;
	const char *order_id; // Tag 37
	size_t order_id_len;
	const char *exec_id; // Tag 17
	size_t exec_id_len;
	const char *symbol; // Tag 55
	size_t symbol_len;
	const char *side; // Tag 54
	size_t side_len;
	const char *exec_type; // Tag 150
	size_t exec_type_len;
	const char *ord_status; // Tag 39
	size_t ord_status_len;
	const char *price; // Tag 44
	size_t price_len;
	const char *order_qty; // Tag 38
	size_t order_qty_len;
	const char *cum_qty; // Tag 14
	size_t cum_qty_len;
	const char *leaves_qty; // Tag 151
	size_t leaves_qty_len;
	const char *last_px; // Tag 31
	size_t last_px_len;
	const char *last_qty; // Tag 32
	size_t last_qty_len;
	const char *text; // Tag 58
	size_t text_len;

	// Prefix (everything before "8=" in the line)
	const char *prefix;
	size_t prefix_len;

	// All other tags (parsed on demand)
	struct TagValue {
		const char *data;
		size_t len;
	};
	std::unordered_map<int, TagValue> other_tags;

	// Ordered list of all tags for group parsing
	// Each pair is (tag_number, TagValue)
	// Preserves original message order needed for repeating groups
	std::vector<std::pair<int, TagValue>> all_tags_ordered;

	// Raw message for debugging/logging
	const char *raw_message;
	size_t raw_message_len;

	// Parse error (if any)
	std::string parse_error;

	// Constructor
	ParsedFixMessage() {
		clear();
	}

	// Clear for reuse
	void clear() {
		msg_type = nullptr;
		msg_type_len = 0;
		sender_comp_id = nullptr;
		sender_comp_id_len = 0;
		target_comp_id = nullptr;
		target_comp_id_len = 0;
		msg_seq_num = nullptr;
		msg_seq_num_len = 0;
		sending_time = nullptr;
		sending_time_len = 0;
		cl_ord_id = nullptr;
		cl_ord_id_len = 0;
		order_id = nullptr;
		order_id_len = 0;
		exec_id = nullptr;
		exec_id_len = 0;
		symbol = nullptr;
		symbol_len = 0;
		side = nullptr;
		side_len = 0;
		exec_type = nullptr;
		exec_type_len = 0;
		ord_status = nullptr;
		ord_status_len = 0;
		price = nullptr;
		price_len = 0;
		order_qty = nullptr;
		order_qty_len = 0;
		cum_qty = nullptr;
		cum_qty_len = 0;
		leaves_qty = nullptr;
		leaves_qty_len = 0;
		last_px = nullptr;
		last_px_len = 0;
		last_qty = nullptr;
		last_qty_len = 0;
		text = nullptr;
		text_len = 0;
		prefix = nullptr;
		prefix_len = 0;
		other_tags.clear();
		all_tags_ordered.clear();
		raw_message = nullptr;
		raw_message_len = 0;
		parse_error.clear();
	}
};
