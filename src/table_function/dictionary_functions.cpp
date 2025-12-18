#include "dictionary_functions.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/common/file_system.hpp"
#include "dictionary/fix_dictionary.hpp"
#include "dictionary/xml_loader.hpp"
#include <unordered_set>

namespace duckdb {

// =============================================================================
// 1. fix_fields(dictionary) - Returns all field definitions
// =============================================================================

struct FixFieldsBindData : public TableFunctionData {
	shared_ptr<FixDictionary> dictionary;
};

struct FixFieldsGlobalState : public GlobalTableFunctionState {
	std::vector<std::pair<int, FixFieldDef>> field_list;
	idx_t current_idx;

	FixFieldsGlobalState() : current_idx(0) {
	}

	idx_t MaxThreads() const override {
		return 1;
	}
};

static unique_ptr<FunctionData> FixFieldsBind(ClientContext &context, TableFunctionBindInput &input,
                                              vector<LogicalType> &return_types, vector<string> &names) {
	auto result = make_uniq<FixFieldsBindData>();

	if (input.inputs.empty()) {
		throw BinderException("fix_fields requires a dictionary file path argument");
	}

	auto &dict_path = StringValue::Get(input.inputs[0]);

	// Load FIX dictionary
	try {
		auto dict = FixDictionaryLoader::LoadBase(context, dict_path);
		result->dictionary = make_shared_ptr<FixDictionary>(std::move(dict));
	} catch (const std::exception &e) {
		throw BinderException("Failed to load FIX dictionary from '%s': %s", dict_path.c_str(), e.what());
	}

	// Define schema
	names.emplace_back("tag");
	return_types.emplace_back(LogicalType(LogicalTypeId::INTEGER));

	names.emplace_back("name");
	return_types.emplace_back(LogicalType(LogicalTypeId::VARCHAR));

	names.emplace_back("type");
	return_types.emplace_back(LogicalType(LogicalTypeId::VARCHAR));

	names.emplace_back("enum_values");
	// LIST<STRUCT(enum VARCHAR, description VARCHAR)>
	child_list_t<LogicalType> struct_children;
	struct_children.push_back(make_pair("enum", LogicalType(LogicalTypeId::VARCHAR)));
	struct_children.push_back(make_pair("description", LogicalType(LogicalTypeId::VARCHAR)));
	return_types.emplace_back(LogicalType::LIST(LogicalType::STRUCT(struct_children)));

	return std::move(result);
}

static unique_ptr<GlobalTableFunctionState> FixFieldsInitGlobal(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<FixFieldsBindData>();
	auto result = make_uniq<FixFieldsGlobalState>();

	// Copy fields to vector for iteration
	for (const auto &[tag, field_def] : bind_data.dictionary->fields) {
		result->field_list.emplace_back(tag, field_def);
	}

	// Sort by tag for consistent ordering
	std::sort(
	    result->field_list.begin(), result->field_list.end(),
	    [](const std::pair<int, FixFieldDef> &a, const std::pair<int, FixFieldDef> &b) { return a.first < b.first; });

	return std::move(result);
}

static void FixFieldsScan(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind_data = data_p.bind_data->Cast<FixFieldsBindData>();
	auto &gstate = data_p.global_state->Cast<FixFieldsGlobalState>();

	idx_t output_idx = 0;

	while (output_idx < STANDARD_VECTOR_SIZE && gstate.current_idx < gstate.field_list.size()) {
		const auto &[tag, field_def] = gstate.field_list[gstate.current_idx];

		// Column 0: tag
		output.data[0].SetValue(output_idx, Value::INTEGER(tag));

		// Column 1: name
		output.data[1].SetValue(output_idx, Value(field_def.name));

		// Column 2: type
		output.data[2].SetValue(output_idx, Value(field_def.type));

		// Column 3: enum_values (LIST<STRUCT>)
		if (field_def.enums.empty()) {
			output.data[3].SetValue(output_idx, Value()); // NULL
		} else {
			vector<Value> enum_list;
			for (const auto &enum_val : field_def.enums) {
				child_list_t<Value> struct_values;
				struct_values.push_back(make_pair("enum", Value(enum_val.enum_value)));
				struct_values.push_back(make_pair("description", Value(enum_val.description)));
				enum_list.push_back(Value::STRUCT(struct_values));
			}

			child_list_t<LogicalType> struct_children;
			struct_children.push_back(make_pair("enum", LogicalType(LogicalTypeId::VARCHAR)));
			struct_children.push_back(make_pair("description", LogicalType(LogicalTypeId::VARCHAR)));
			auto struct_type = LogicalType::STRUCT(struct_children);

			output.data[3].SetValue(output_idx, Value::LIST(struct_type, enum_list));
		}

		output_idx++;
		gstate.current_idx++;
	}

	output.SetCardinality(output_idx);
}

TableFunction FixFieldsFunction::GetFunction() {
	TableFunction func("fix_fields", {LogicalType(LogicalTypeId::VARCHAR)}, FixFieldsScan, FixFieldsBind, FixFieldsInitGlobal);
	func.name = "fix_fields";
	return func;
}

// =============================================================================
// 2. fix_message_fields(dictionary) - Returns fields used by each message
// =============================================================================

struct FixMessageFieldsBindData : public TableFunctionData {
	shared_ptr<FixDictionary> dictionary;
};

struct MessageFieldEntry {
	string msgtype;
	string msg_name;
	string category;
	int tag;
	string field_name;
	bool required;
	int group_id; // -1 if not in a group
};

struct FixMessageFieldsGlobalState : public GlobalTableFunctionState {
	std::vector<MessageFieldEntry> entries;
	idx_t current_idx;

	FixMessageFieldsGlobalState() : current_idx(0) {
	}

	idx_t MaxThreads() const override {
		return 1;
	}
};

// Helper to recursively add group fields
static void AddGroupFields(const FixGroupDef &group_def, const string &msgtype, const string &msg_name,
                           int parent_group_id, const FixDictionary &dict, std::vector<MessageFieldEntry> &entries) {
	for (int field_tag : group_def.field_tags) {
		auto field_it = dict.fields.find(field_tag);
		string field_name = field_it != dict.fields.end() ? field_it->second.name : "Unknown";

		entries.push_back({msgtype, msg_name, "group", field_tag, field_name,
		                   false, // Group fields are typically not "required" in the same sense
		                   parent_group_id});
	}

	// Recursively add nested groups
	for (const auto &[sub_count_tag, sub_group] : group_def.subgroups) {
		AddGroupFields(sub_group, msgtype, msg_name, sub_count_tag, dict, entries);
	}
}

static unique_ptr<FunctionData> FixMessageFieldsBind(ClientContext &context, TableFunctionBindInput &input,
                                                     vector<LogicalType> &return_types, vector<string> &names) {
	auto result = make_uniq<FixMessageFieldsBindData>();

	if (input.inputs.empty()) {
		throw BinderException("fix_message_fields requires a dictionary file path argument");
	}

	auto &dict_path = StringValue::Get(input.inputs[0]);

	// Load FIX dictionary
	try {
		auto dict = FixDictionaryLoader::LoadBase(context, dict_path);
		result->dictionary = make_shared_ptr<FixDictionary>(std::move(dict));
	} catch (const std::exception &e) {
		throw BinderException("Failed to load FIX dictionary from '%s': %s", dict_path.c_str(), e.what());
	}

	// Define schema
	names.emplace_back("msgtype");
	return_types.emplace_back(LogicalType(LogicalTypeId::VARCHAR));

	names.emplace_back("name");
	return_types.emplace_back(LogicalType(LogicalTypeId::VARCHAR));

	names.emplace_back("category");
	return_types.emplace_back(LogicalType(LogicalTypeId::VARCHAR));

	names.emplace_back("tag");
	return_types.emplace_back(LogicalType(LogicalTypeId::INTEGER));

	names.emplace_back("field_name");
	return_types.emplace_back(LogicalType(LogicalTypeId::VARCHAR));

	names.emplace_back("required");
	return_types.emplace_back(LogicalType(LogicalTypeId::BOOLEAN));

	names.emplace_back("group_id");
	return_types.emplace_back(LogicalType(LogicalTypeId::INTEGER));

	return std::move(result);
}

static unique_ptr<GlobalTableFunctionState> FixMessageFieldsInitGlobal(ClientContext &context,
                                                                       TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<FixMessageFieldsBindData>();
	auto result = make_uniq<FixMessageFieldsGlobalState>();

	// Iterate through all messages
	for (const auto &[msgtype, msg_def] : bind_data.dictionary->messages) {
		// Add required fields
		for (int tag : msg_def.required_fields) {
			auto field_it = bind_data.dictionary->fields.find(tag);
			string field_name = field_it != bind_data.dictionary->fields.end() ? field_it->second.name : "Unknown";

			result->entries.push_back({
			    msgtype, msg_def.name, "required", tag, field_name, true,
			    -1 // Not in a group
			});
		}

		// Add optional fields
		for (int tag : msg_def.optional_fields) {
			auto field_it = bind_data.dictionary->fields.find(tag);
			string field_name = field_it != bind_data.dictionary->fields.end() ? field_it->second.name : "Unknown";

			result->entries.push_back({
			    msgtype, msg_def.name, "optional", tag, field_name, false,
			    -1 // Not in a group
			});
		}

		// Add group fields recursively
		for (const auto &[count_tag, group_def] : msg_def.groups) {
			AddGroupFields(group_def, msgtype, msg_def.name, count_tag, *bind_data.dictionary, result->entries);
		}
	}

	return std::move(result);
}

static void FixMessageFieldsScan(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &gstate = data_p.global_state->Cast<FixMessageFieldsGlobalState>();

	idx_t output_idx = 0;

	while (output_idx < STANDARD_VECTOR_SIZE && gstate.current_idx < gstate.entries.size()) {
		const auto &entry = gstate.entries[gstate.current_idx];

		// Column 0: msgtype
		output.data[0].SetValue(output_idx, Value(entry.msgtype));

		// Column 1: name (message name)
		output.data[1].SetValue(output_idx, Value(entry.msg_name));

		// Column 2: category
		output.data[2].SetValue(output_idx, Value(entry.category));

		// Column 3: tag
		output.data[3].SetValue(output_idx, Value::INTEGER(entry.tag));

		// Column 4: field_name
		output.data[4].SetValue(output_idx, Value(entry.field_name));

		// Column 5: required
		output.data[5].SetValue(output_idx, Value::BOOLEAN(entry.required));

		// Column 6: group_id
		if (entry.group_id == -1) {
			output.data[6].SetValue(output_idx, Value()); // NULL
		} else {
			output.data[6].SetValue(output_idx, Value::INTEGER(entry.group_id));
		}

		output_idx++;
		gstate.current_idx++;
	}

	output.SetCardinality(output_idx);
}

TableFunction FixMessageFieldsFunction::GetFunction() {
	TableFunction func("fix_message_fields", {LogicalType(LogicalTypeId::VARCHAR)}, FixMessageFieldsScan, FixMessageFieldsBind,
	                   FixMessageFieldsInitGlobal);
	func.name = "fix_message_fields";
	return func;
}

// =============================================================================
// 3. fix_groups(dictionary) - Returns all repeating group definitions
// =============================================================================

struct FixGroupsBindData : public TableFunctionData {
	shared_ptr<FixDictionary> dictionary;
};

struct GroupEntry {
	int group_tag;
	std::vector<int> field_tags;
	std::vector<string> message_types;
	string name;
};

struct FixGroupsGlobalState : public GlobalTableFunctionState {
	std::vector<GroupEntry> entries;
	idx_t current_idx;

	FixGroupsGlobalState() : current_idx(0) {
	}

	idx_t MaxThreads() const override {
		return 1;
	}
};

// Helper to recursively collect groups
static void CollectGroups(const FixGroupDef &group_def, int count_tag, const string &msgtype,
                          std::unordered_map<int, GroupEntry> &group_map) {
	// Add this group
	auto &entry = group_map[count_tag];
	entry.group_tag = count_tag;
	entry.field_tags = group_def.field_tags;
	entry.message_types.push_back(msgtype);

	// Recursively collect nested groups
	for (const auto &[sub_count_tag, sub_group] : group_def.subgroups) {
		CollectGroups(sub_group, sub_count_tag, msgtype, group_map);
	}
}

static unique_ptr<FunctionData> FixGroupsBind(ClientContext &context, TableFunctionBindInput &input,
                                              vector<LogicalType> &return_types, vector<string> &names) {
	auto result = make_uniq<FixGroupsBindData>();

	if (input.inputs.empty()) {
		throw BinderException("fix_groups requires a dictionary file path argument");
	}

	auto &dict_path = StringValue::Get(input.inputs[0]);

	// Load FIX dictionary
	try {
		auto dict = FixDictionaryLoader::LoadBase(context, dict_path);
		result->dictionary = make_shared_ptr<FixDictionary>(std::move(dict));
	} catch (const std::exception &e) {
		throw BinderException("Failed to load FIX dictionary from '%s': %s", dict_path.c_str(), e.what());
	}

	// Define schema
	names.emplace_back("group_tag");
	return_types.emplace_back(LogicalType(LogicalTypeId::INTEGER));

	names.emplace_back("field_tag");
	return_types.emplace_back(LogicalType::LIST(LogicalType(LogicalTypeId::INTEGER)));

	names.emplace_back("message_types");
	return_types.emplace_back(LogicalType::LIST(LogicalType(LogicalTypeId::VARCHAR)));

	names.emplace_back("name");
	return_types.emplace_back(LogicalType(LogicalTypeId::VARCHAR));

	return std::move(result);
}

static unique_ptr<GlobalTableFunctionState> FixGroupsInitGlobal(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<FixGroupsBindData>();
	auto result = make_uniq<FixGroupsGlobalState>();

	// Build map of group_tag -> GroupEntry (with aggregated message_types)
	std::unordered_map<int, GroupEntry> group_map;

	// Iterate through all messages and collect groups
	for (const auto &[msgtype, msg_def] : bind_data.dictionary->messages) {
		for (const auto &[count_tag, group_def] : msg_def.groups) {
			CollectGroups(group_def, count_tag, msgtype, group_map);
		}
	}

	// Convert map to vector and look up group names
	for (auto &[group_tag, entry] : group_map) {
		auto field_it = bind_data.dictionary->fields.find(group_tag);
		if (field_it != bind_data.dictionary->fields.end()) {
			entry.name = field_it->second.name;
		} else {
			entry.name = "Unknown";
		}

		// Remove duplicate message types
		std::sort(entry.message_types.begin(), entry.message_types.end());
		entry.message_types.erase(std::unique(entry.message_types.begin(), entry.message_types.end()),
		                          entry.message_types.end());

		result->entries.push_back(std::move(entry));
	}

	// Sort by group_tag for consistent ordering
	std::sort(result->entries.begin(), result->entries.end(),
	          [](const GroupEntry &a, const GroupEntry &b) { return a.group_tag < b.group_tag; });

	return std::move(result);
}

static void FixGroupsScan(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &gstate = data_p.global_state->Cast<FixGroupsGlobalState>();

	idx_t output_idx = 0;

	while (output_idx < STANDARD_VECTOR_SIZE && gstate.current_idx < gstate.entries.size()) {
		const auto &entry = gstate.entries[gstate.current_idx];

		// Column 0: group_tag
		output.data[0].SetValue(output_idx, Value::INTEGER(entry.group_tag));

		// Column 1: field_tag (LIST<INT>)
		vector<Value> field_tag_list;
		for (int tag : entry.field_tags) {
			field_tag_list.push_back(Value::INTEGER(tag));
		}
		output.data[1].SetValue(output_idx, Value::LIST(LogicalType(LogicalTypeId::INTEGER), field_tag_list));

		// Column 2: message_types (LIST<VARCHAR>)
		vector<Value> msg_type_list;
		for (const string &msgtype : entry.message_types) {
			msg_type_list.push_back(Value(msgtype));
		}
		output.data[2].SetValue(output_idx, Value::LIST(LogicalType(LogicalTypeId::VARCHAR), msg_type_list));

		// Column 3: name
		output.data[3].SetValue(output_idx, Value(entry.name));

		output_idx++;
		gstate.current_idx++;
	}

	output.SetCardinality(output_idx);
}

TableFunction FixGroupsFunction::GetFunction() {
	TableFunction func("fix_groups", {LogicalType(LogicalTypeId::VARCHAR)}, FixGroupsScan, FixGroupsBind, FixGroupsInitGlobal);
	func.name = "fix_groups";
	return func;
}

} // namespace duckdb
