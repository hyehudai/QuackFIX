#include "fix_tokenizer.hpp"
#include "fix_hot_tags.hpp"
#include <cstring>
#include <cstdlib>

bool FixTokenizer::IsNumeric(const char *str, size_t len) {
	if (len == 0) {
		return false;
	}
	for (size_t i = 0; i < len; i++) {
		if (str[i] < '0' || str[i] > '9') {
			return false;
		}
	}
	return true;
}

bool FixTokenizer::ExtractTagNumber(const char *tag_str, size_t tag_len, int &tag_out) {
	if (!IsNumeric(tag_str, tag_len)) {
		return false;
	}

	// Convert to int manually
	tag_out = 0;
	for (size_t i = 0; i < tag_len; i++) {
		tag_out = tag_out * 10 + (tag_str[i] - '0');
	}
	return true;
}

bool FixTokenizer::ParseTag(const char *tag_str, size_t tag_len, const char *value, size_t value_len,
                            ParsedFixMessage &msg) {
	int tag;
	if (!ExtractTagNumber(tag_str, tag_len, tag)) {
		return false;
	}

	// Add ALL tags to ordered list (for group parsing)
	msg.all_tags_ordered.push_back({tag, {value, value_len}});

	// Store in appropriate field based on tag number
	// Using centralized FixHotTags constants for maintainability
	switch (tag) {
	case duckdb::FixHotTags::MSG_TYPE:
		msg.msg_type = value;
		msg.msg_type_len = value_len;
		break;
	case duckdb::FixHotTags::SENDER_COMP_ID:
		msg.sender_comp_id = value;
		msg.sender_comp_id_len = value_len;
		break;
	case duckdb::FixHotTags::TARGET_COMP_ID:
		msg.target_comp_id = value;
		msg.target_comp_id_len = value_len;
		break;
	case duckdb::FixHotTags::MSG_SEQ_NUM:
		msg.msg_seq_num = value;
		msg.msg_seq_num_len = value_len;
		break;
	case duckdb::FixHotTags::SENDING_TIME:
		msg.sending_time = value;
		msg.sending_time_len = value_len;
		break;
	case duckdb::FixHotTags::CL_ORD_ID:
		msg.cl_ord_id = value;
		msg.cl_ord_id_len = value_len;
		break;
	case duckdb::FixHotTags::ORDER_ID:
		msg.order_id = value;
		msg.order_id_len = value_len;
		break;
	case duckdb::FixHotTags::EXEC_ID:
		msg.exec_id = value;
		msg.exec_id_len = value_len;
		break;
	case duckdb::FixHotTags::SYMBOL:
		msg.symbol = value;
		msg.symbol_len = value_len;
		break;
	case duckdb::FixHotTags::SIDE:
		msg.side = value;
		msg.side_len = value_len;
		break;
	case duckdb::FixHotTags::EXEC_TYPE:
		msg.exec_type = value;
		msg.exec_type_len = value_len;
		break;
	case duckdb::FixHotTags::ORD_STATUS:
		msg.ord_status = value;
		msg.ord_status_len = value_len;
		break;
	case duckdb::FixHotTags::PRICE:
		msg.price = value;
		msg.price_len = value_len;
		break;
	case duckdb::FixHotTags::ORDER_QTY:
		msg.order_qty = value;
		msg.order_qty_len = value_len;
		break;
	case duckdb::FixHotTags::CUM_QTY:
		msg.cum_qty = value;
		msg.cum_qty_len = value_len;
		break;
	case duckdb::FixHotTags::LEAVES_QTY:
		msg.leaves_qty = value;
		msg.leaves_qty_len = value_len;
		break;
	case duckdb::FixHotTags::LAST_PX:
		msg.last_px = value;
		msg.last_px_len = value_len;
		break;
	case duckdb::FixHotTags::LAST_QTY:
		msg.last_qty = value;
		msg.last_qty_len = value_len;
		break;
	case duckdb::FixHotTags::TEXT:
		msg.text = value;
		msg.text_len = value_len;
		break;
	default:
		// Store in other_tags map
		msg.other_tags[tag] = {value, value_len};
		break;
	}

	return true;
}

bool FixTokenizer::Parse(const char *input, size_t input_len, ParsedFixMessage &msg, char delimiter) {
	msg.clear();
	msg.raw_message = input;
	msg.raw_message_len = input_len;

	if (input_len == 0) {
		msg.parse_error = "Empty message";
		return false;
	}

	size_t pos = 0;
	size_t tag_count = 0;

	while (pos < input_len) {
		// Find next delimiter
		size_t next_delim = pos;
		while (next_delim < input_len && input[next_delim] != delimiter) {
			next_delim++;
		}

		// Extract tag=value pair
		size_t pair_len = next_delim - pos;

		if (pair_len > 0) {
			const char *pair_start = input + pos;

			// Find '=' separator
			size_t eq_pos = 0;
			while (eq_pos < pair_len && pair_start[eq_pos] != '=') {
				eq_pos++;
			}

			if (eq_pos >= pair_len) {
				msg.parse_error = "Invalid tag format (missing '=')";
				return false;
			}

			const char *tag_str = pair_start;
			size_t tag_len = eq_pos;
			const char *value = pair_start + eq_pos + 1;
			size_t value_len = pair_len - eq_pos - 1;

			if (!ParseTag(tag_str, tag_len, value, value_len, msg)) {
				msg.parse_error = "Failed to parse tag";
				return false;
			}

			tag_count++;
		}

		pos = next_delim + 1;
	}

	if (tag_count == 0) {
		msg.parse_error = "No valid tags found";
		return false;
	}

	// Validate that we at least have MsgType (tag 35)
	if (msg.msg_type == nullptr || msg.msg_type_len == 0) {
		msg.parse_error = "Missing required tag 35 (MsgType)";
		return false;
	}

	return true;
}
