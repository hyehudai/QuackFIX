#include "fix_group_parser.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/types.hpp"
#include <algorithm>

namespace duckdb {

bool FixGroupParser::IsGroupField(int tag, const std::vector<int> &group_field_tags) {
	for (int gf : group_field_tags) {
		if (tag == gf) {
			return true;
		}
	}
	return false;
}

int FixGroupParser::GetGroupCount(const std::unordered_map<int, ParsedFixMessage::TagValue> &other_tags,
                                  int count_tag) {
	auto tag_it = other_tags.find(count_tag);
	if (tag_it == other_tags.end()) {
		return 0; // Group not present
	}

	try {
		std::string count_str(tag_it->second.data, tag_it->second.len);
		int count = std::stoi(count_str);

		// Sanity check: reject invalid or excessive counts
		if (count <= 0 || count > 100) {
			return 0;
		}

		return count;
	} catch (...) {
		return 0; // Invalid count
	}
}

size_t FixGroupParser::FindCountTagPosition(const std::vector<std::pair<int, ParsedFixMessage::TagValue>> &ordered_tags,
                                            int count_tag) {
	for (size_t i = 0; i < ordered_tags.size(); i++) {
		if (ordered_tags[i].first == count_tag) {
			return i;
		}
	}
	return ordered_tags.size(); // Not found
}

vector<Value>
FixGroupParser::ParseGroupInstances(const std::vector<std::pair<int, ParsedFixMessage::TagValue>> &ordered_tags,
                                    size_t start_pos, int group_count, const std::vector<int> &group_field_tags) {
	vector<Value> group_instances;
	size_t pos = start_pos;

	for (int instance = 0; instance < group_count && pos < ordered_tags.size(); instance++) {
		// Parse one group instance
		vector<Value> instance_map_entries;

		// Collect tags that belong to this group instance
		while (pos < ordered_tags.size()) {
			int tag = ordered_tags[pos].first;

			// Check if this tag belongs to the current group
			if (!IsGroupField(tag, group_field_tags)) {
				// Not a group field - either another group starts or non-group tag
				break;
			}

			// Add this tag to the instance
			auto &tag_value = ordered_tags[pos].second;
			child_list_t<Value> map_entry;
			map_entry.push_back(make_pair("key", Value::INTEGER(tag)));
			map_entry.push_back(make_pair("value", Value(std::string(tag_value.data, tag_value.len))));
			instance_map_entries.push_back(Value::STRUCT(map_entry));

			pos++;

			// Check if we've seen the first field again (marks next instance)
			if (pos < ordered_tags.size() && !group_field_tags.empty() &&
			    ordered_tags[pos].first == group_field_tags[0]) {
				break;
			}
		}

		if (!instance_map_entries.empty()) {
			// Create MAP for this instance
			auto instance_map_type = LogicalType::MAP(LogicalType::INTEGER, LogicalType::VARCHAR);
			auto instance_child_type = ListType::GetChildType(instance_map_type);
			group_instances.push_back(Value::MAP(instance_child_type, instance_map_entries));
		}
	}

	return group_instances;
}

Value FixGroupParser::ParseGroups(const ParsedFixMessage &parsed, const FixDictionary &dict, bool needs_groups) {
	// Early exit optimization - groups not requested
	if (!needs_groups) {
		return Value(); // NULL
	}

	// Validate prerequisites
	if (parsed.all_tags_ordered.empty() || parsed.msg_type == nullptr || parsed.msg_type_len == 0) {
		return Value(); // NULL
	}

	// Look up message type in dictionary
	std::string msg_type_str(parsed.msg_type, parsed.msg_type_len);
	auto msg_it = dict.messages.find(msg_type_str);

	if (msg_it == dict.messages.end()) {
		// Message type not in dictionary - no groups to parse
		return Value();
	}

	const auto &message_def = msg_it->second;
	vector<Value> outer_map_entries;

	// Iterate through all groups defined for this message type
	for (const auto &[count_tag, group_def] : message_def.groups) {
		// Check if this group exists in the message
		int group_count = GetGroupCount(parsed.other_tags, count_tag);
		if (group_count == 0) {
			continue; // Group not present or invalid count
		}

		// Get field tags from dictionary
		const std::vector<int> &group_field_tags = group_def.field_tags;
		if (group_field_tags.empty()) {
			continue; // No fields defined for this group
		}

		// Find the position of the count tag in ordered list
		size_t count_tag_pos = FindCountTagPosition(parsed.all_tags_ordered, count_tag);
		if (count_tag_pos >= parsed.all_tags_ordered.size()) {
			continue; // Count tag not found in ordered list
		}

		// Parse group instances from ordered tags starting after count tag
		auto group_instances =
		    ParseGroupInstances(parsed.all_tags_ordered, count_tag_pos + 1, group_count, group_field_tags);

		if (!group_instances.empty()) {
			// Create outer map entry for this group
			child_list_t<Value> outer_struct;
			outer_struct.push_back(make_pair("key", Value::INTEGER(count_tag)));

			// Create LIST value with the correct child type
			auto instance_map_type = LogicalType::MAP(LogicalType::INTEGER, LogicalType::VARCHAR);
			outer_struct.push_back(make_pair("value", Value::LIST(instance_map_type, group_instances)));
			outer_map_entries.push_back(Value::STRUCT(outer_struct));
		}
	}

	if (outer_map_entries.empty()) {
		return Value(); // NULL if no groups found
	}

	// Create final nested MAP
	auto outer_map_type = LogicalType::MAP(
	    LogicalType::INTEGER, LogicalType::LIST(LogicalType::MAP(LogicalType::INTEGER, LogicalType::VARCHAR)));
	auto outer_child_type = ListType::GetChildType(outer_map_type);
	return Value::MAP(outer_child_type, outer_map_entries);
}

} // namespace duckdb
