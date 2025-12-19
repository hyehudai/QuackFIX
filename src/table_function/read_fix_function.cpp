#include "read_fix_function.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/time.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/function/table/read_csv.hpp"
#include "dictionary/fix_dictionary.hpp"
#include "dictionary/xml_loader.hpp"
#include "dictionary/embedded_fix44_dictionary.hpp"
#include "parser/fix_tokenizer.hpp"
#include "parser/fix_message.hpp"
#include "parser/fix_type_conversions.hpp"
#include "parser/fix_group_parser.hpp"
#include "parser/fix_file_reader.hpp"
#include "parser/fix_hot_tags.hpp"
#include <sstream>

namespace duckdb {

// Bind data - configuration for the table function
struct ReadFixBindData : public TableFunctionData {
	vector<string> files;
	shared_ptr<FixDictionary> dictionary;

	// Phase 7.5: Custom tag support (rtags + tagIds parameters)
	vector<pair<string, int>> custom_tags; // {tag_name, tag_number}

	// Phase 7.7: Delimiter parameter
	char delimiter = '|'; // Default to pipe

	ReadFixBindData() {
	}
};

// Global state - shared across all threads
struct ReadFixGlobalState : public GlobalTableFunctionState {
	idx_t file_index;
	mutex lock;

	// Phase 7.5: Projection pushdown support
	vector<idx_t> projection_ids;
	vector<ColumnIndex> column_indexes;
	bool needs_tags;
	bool needs_groups;

	ReadFixGlobalState() : file_index(0), needs_tags(true), needs_groups(true) {
	}

	idx_t MaxThreads() const override {
		return 1; // Single-threaded for Phase 3
	}

	bool CanRemoveFilterColumns() const {
		return !projection_ids.empty();
	}

	bool IsColumnNeeded(idx_t col_idx) const {
		// Column is needed if it's in projection_ids or column_indexes (filter columns)
		if (projection_ids.empty()) {
			return true; // No projection pushdown, need all columns
		}
		// Check if in projection
		for (auto proj_id : projection_ids) {
			if (proj_id == col_idx) {
				return true;
			}
		}
		// Check if in column_indexes (includes filter columns)
		for (idx_t i = 0; i < column_indexes.size(); i++) {
			if (column_indexes[i].GetPrimaryIndex() == col_idx) {
				return true;
			}
		}
		return false;
	}
};

// Column writer helper - encapsulates output column writing logic
struct FixColumnWriter {
	DataChunk &output;
	idx_t row_idx;
	const ReadFixBindData &bind_data;
	const ReadFixGlobalState &gstate;
	vector<string> &conversion_errors;

	FixColumnWriter(DataChunk &out, idx_t row, const ReadFixBindData &bind, const ReadFixGlobalState &gs,
	                vector<string> &errors)
	    : output(out), row_idx(row), bind_data(bind), gstate(gs), conversion_errors(errors) {
	}

	// Get output column index from schema column index (handles projection pushdown)
	idx_t GetOutputIdx(idx_t schema_col_idx) const {
		for (idx_t i = 0; i < gstate.column_indexes.size(); i++) {
			if (gstate.column_indexes[i].GetPrimaryIndex() == schema_col_idx) {
				return i;
			}
		}
		return DConstants::INVALID_INDEX;
	}

	// Write all hot tags (columns 0-18)
	void WriteHotTags(const ParsedFixMessage &parsed);

	// Write tags MAP column (column 19)
	void WriteTagsMap(const ParsedFixMessage &parsed);

	// Write groups MAP column (column 20)
	void WriteGroupsMap(const ParsedFixMessage &parsed);

	// Write metadata columns (raw_message, parse_error) - columns 21-22
	void WriteMetadata(const string &raw_line);

	// Write custom tag columns (columns 23+)
	void WriteCustomTags(const ParsedFixMessage &parsed);
};

// Local state - per-thread state
struct ReadFixLocalState : public LocalTableFunctionState {
	FixFileReader file_reader;

	ReadFixLocalState() {
	}
};

// Bind function - called once at query planning time
static unique_ptr<FunctionData> ReadFixBind(ClientContext &context, TableFunctionBindInput &input,
                                            vector<LogicalType> &return_types, vector<string> &names) {
	auto result = make_uniq<ReadFixBindData>();

	// Get file path parameter
	if (input.inputs.empty()) {
		throw BinderException("read_fix requires at least one argument (file path)");
	}

	auto &file_path = StringValue::Get(input.inputs[0]);

	// Expand glob patterns using DuckDB FileSystem
	auto &fs = FileSystem::GetFileSystem(context);
	auto file_list = fs.GlobFiles(file_path, context, FileGlobOptions::DISALLOW_EMPTY);

	// Extract file paths from OpenFileInfo objects
	for (auto &file_info : file_list) {
		result->files.push_back(file_info.path);
	}

	// Load FIX dictionary for group parsing and custom tag validation
	try {
		FixDictionary dict;
		
		if (input.named_parameters.find("dictionary") != input.named_parameters.end()) {
			// User provided a dictionary path - load from file
			string dict_path = StringValue::Get(input.named_parameters.at("dictionary"));
			dict = FixDictionaryLoader::LoadBase(context, dict_path);
		} else {
			// No dictionary provided - use embedded FIX 4.4 dictionary
			dict = FixDictionaryLoader::LoadFromString(EMBEDDED_FIX44_DICTIONARY);
		}
		
		result->dictionary = make_shared_ptr<FixDictionary>(std::move(dict));
	} catch (const std::exception &e) {
		throw BinderException("Failed to load FIX dictionary: %s", e.what());
	}

	// Phase 7.7: Parse delimiter parameter
	if (input.named_parameters.find("delimiter") != input.named_parameters.end()) {
		string delim_str = StringValue::Get(input.named_parameters.at("delimiter"));
		if (delim_str.empty()) {
			throw BinderException("delimiter cannot be empty");
		} else if (delim_str.size() == 1) {
			result->delimiter = delim_str[0];
		} else if (delim_str == "\\x01") {
			result->delimiter = '\x01'; // SOH
		} else {
			throw BinderException("delimiter must be a single character or '\\x01' for SOH");
		}
	}

	// Phase 7.5: Process custom tag parameters (rtags and tagIds)
	// Use a set to track already-added tags (avoid duplicates)
	std::unordered_set<int> added_tags;

	// Process rtags parameter (tag names)
	if (input.named_parameters.find("rtags") != input.named_parameters.end()) {
		if (!result->dictionary) {
			throw BinderException("Cannot use rtags parameter: FIX dictionary failed to load");
		}

		auto &rtags_value = input.named_parameters.at("rtags");
		auto &rtags_list = ListValue::GetChildren(rtags_value);

		for (auto &tag_name_value : rtags_list) {
			string tag_name = StringValue::Get(tag_name_value);

			// Validate tag name exists in dictionary
			auto it = result->dictionary->name_to_tag.find(tag_name);
			if (it == result->dictionary->name_to_tag.end()) {
				throw BinderException("Invalid tag name in rtags: '%s'. Tag not found in FIX dictionary.",
				                      tag_name.c_str());
			}

			int tag_num = it->second;

			// Add to custom tags if not already added
			if (added_tags.find(tag_num) == added_tags.end()) {
				result->custom_tags.push_back({tag_name, tag_num});
				added_tags.insert(tag_num);
			}
		}
	}

	// Process tagIds parameter (tag numbers)
	if (input.named_parameters.find("tagIds") != input.named_parameters.end()) {
		if (!result->dictionary) {
			throw BinderException("Cannot use tagIds parameter: FIX dictionary failed to load");
		}

		auto &tagIds_value = input.named_parameters.at("tagIds");
		auto &tagIds_list = ListValue::GetChildren(tagIds_value);

		for (auto &tag_id_value : tagIds_list) {
			int tag_num = IntegerValue::Get(tag_id_value);

			// Phase 7.7: Allow unknown tags - name them "TagXX"
			string tag_name;
			auto it = result->dictionary->fields.find(tag_num);
			if (it == result->dictionary->fields.end()) {
				// Unknown tag - use "TagXX" format
				tag_name = "Tag" + std::to_string(tag_num);
			} else {
				// Known tag - use dictionary name
				tag_name = it->second.name;
			}

			// Add to custom tags if not already added
			if (added_tags.find(tag_num) == added_tags.end()) {
				result->custom_tags.push_back({tag_name, tag_num});
				added_tags.insert(tag_num);
			}
		}
	}

	// Define full schema for Phase 4.5 - with proper types
	names.emplace_back("MsgType");
	return_types.emplace_back(LogicalType(LogicalTypeId::VARCHAR));

	names.emplace_back("SenderCompID");
	return_types.emplace_back(LogicalType(LogicalTypeId::VARCHAR));

	names.emplace_back("TargetCompID");
	return_types.emplace_back(LogicalType(LogicalTypeId::VARCHAR));

	names.emplace_back("MsgSeqNum");
	return_types.emplace_back(LogicalType(LogicalTypeId::BIGINT)); // Numeric

	names.emplace_back("SendingTime");
	return_types.emplace_back(LogicalType(LogicalTypeId::TIMESTAMP)); // Timestamp with milliseconds

	names.emplace_back("ClOrdID");
	return_types.emplace_back(LogicalType(LogicalTypeId::VARCHAR));

	names.emplace_back("OrderID");
	return_types.emplace_back(LogicalType(LogicalTypeId::VARCHAR));

	names.emplace_back("ExecID");
	return_types.emplace_back(LogicalType(LogicalTypeId::VARCHAR));

	names.emplace_back("Symbol");
	return_types.emplace_back(LogicalType(LogicalTypeId::VARCHAR));

	names.emplace_back("Side");
	return_types.emplace_back(LogicalType(LogicalTypeId::VARCHAR));

	names.emplace_back("ExecType");
	return_types.emplace_back(LogicalType(LogicalTypeId::VARCHAR));

	names.emplace_back("OrdStatus");
	return_types.emplace_back(LogicalType(LogicalTypeId::VARCHAR));

	names.emplace_back("Price");
	return_types.emplace_back(LogicalType(LogicalTypeId::DOUBLE)); // Numeric

	names.emplace_back("OrderQty");
	return_types.emplace_back(LogicalType(LogicalTypeId::DOUBLE)); // Numeric

	names.emplace_back("CumQty");
	return_types.emplace_back(LogicalType(LogicalTypeId::DOUBLE)); // Numeric

	names.emplace_back("LeavesQty");
	return_types.emplace_back(LogicalType(LogicalTypeId::DOUBLE)); // Numeric

	names.emplace_back("LastPx");
	return_types.emplace_back(LogicalType(LogicalTypeId::DOUBLE)); // Numeric

	names.emplace_back("LastQty");
	return_types.emplace_back(LogicalType(LogicalTypeId::DOUBLE)); // Numeric

	names.emplace_back("Text");
	return_types.emplace_back(LogicalType(LogicalTypeId::VARCHAR));

	// Phase 5: Non-hot tags
	names.emplace_back("tags");
	return_types.emplace_back(LogicalType::MAP(LogicalType::INTEGER, LogicalType::VARCHAR));

	// Phase 5: Repeating groups
	names.emplace_back("groups");
	return_types.emplace_back(LogicalType::MAP(
	    LogicalType::INTEGER, LogicalType::LIST(LogicalType::MAP(LogicalType::INTEGER, LogicalType::VARCHAR))));

	names.emplace_back("raw_message");
	return_types.emplace_back(LogicalType(LogicalTypeId::VARCHAR));

	names.emplace_back("parse_error");
	return_types.emplace_back(LogicalType(LogicalTypeId::VARCHAR));

	// Phase 7.5: Add custom tag columns (after standard columns)
	for (const auto &tag_pair : result->custom_tags) {
		names.emplace_back(tag_pair.first);
		return_types.emplace_back(LogicalType(LogicalTypeId::VARCHAR));
	}

	return std::move(result);
}

// InitGlobal - initialize global state
static unique_ptr<GlobalTableFunctionState> ReadFixInitGlobal(ClientContext &context, TableFunctionInitInput &input) {
	auto result = make_uniq<ReadFixGlobalState>();

	// Phase 7.5: Store projection information
	result->projection_ids = input.projection_ids;
	result->column_indexes = input.column_indexes;

	// Determine if tags and groups columns are needed
	// Column 19 is tags, Column 20 is groups (0-indexed)
	result->needs_tags = result->IsColumnNeeded(19);
	result->needs_groups = result->IsColumnNeeded(20);

	return std::move(result);
}

// InitLocal - initialize local state
static unique_ptr<LocalTableFunctionState> ReadFixInitLocal(ExecutionContext &context, TableFunctionInitInput &input,
                                                            GlobalTableFunctionState *global_state) {
	auto result = make_uniq<ReadFixLocalState>();
	return std::move(result);
}

// FixColumnWriter method implementations
void FixColumnWriter::WriteHotTags(const ParsedFixMessage &parsed) {
	// Helper lambdas for setting field values
	auto set_string = [&](idx_t schema_col, const char *ptr, size_t len) {
		auto out_idx = GetOutputIdx(schema_col);
		if (out_idx != DConstants::INVALID_INDEX) {
			SetStringField(output.data[out_idx], row_idx, ptr, len);
		}
	};

	auto set_int64 = [&](idx_t schema_col, const char *ptr, size_t len, const char *name) {
		auto out_idx = GetOutputIdx(schema_col);
		if (out_idx != DConstants::INVALID_INDEX) {
			int64_t val;
			if (ConvertToInt64(ptr, len, val, conversion_errors, name)) {
				output.data[out_idx].SetValue(row_idx, Value::BIGINT(val));
			} else {
				output.data[out_idx].SetValue(row_idx, Value());
			}
		}
	};

	auto set_double = [&](idx_t schema_col, const char *ptr, size_t len, const char *name) {
		auto out_idx = GetOutputIdx(schema_col);
		if (out_idx != DConstants::INVALID_INDEX) {
			double val;
			if (ConvertToDouble(ptr, len, val, conversion_errors, name)) {
				output.data[out_idx].SetValue(row_idx, Value::DOUBLE(val));
			} else {
				output.data[out_idx].SetValue(row_idx, Value());
			}
		}
	};

	auto set_timestamp = [&](idx_t schema_col, const char *ptr, size_t len, const char *name) {
		auto out_idx = GetOutputIdx(schema_col);
		if (out_idx != DConstants::INVALID_INDEX) {
			timestamp_t ts;
			if (ConvertToTimestamp(ptr, len, ts, conversion_errors, name)) {
				output.data[out_idx].SetValue(row_idx, Value::TIMESTAMP(ts));
			} else {
				output.data[out_idx].SetValue(row_idx, Value());
			}
		}
	};

	// Write all 19 hot tag columns (0-18)
	set_string(0, parsed.msg_type, parsed.msg_type_len);
	set_string(1, parsed.sender_comp_id, parsed.sender_comp_id_len);
	set_string(2, parsed.target_comp_id, parsed.target_comp_id_len);
	set_int64(3, parsed.msg_seq_num, parsed.msg_seq_num_len, "MsgSeqNum");
	set_timestamp(4, parsed.sending_time, parsed.sending_time_len, "SendingTime");
	set_string(5, parsed.cl_ord_id, parsed.cl_ord_id_len);
	set_string(6, parsed.order_id, parsed.order_id_len);
	set_string(7, parsed.exec_id, parsed.exec_id_len);
	set_string(8, parsed.symbol, parsed.symbol_len);
	set_string(9, parsed.side, parsed.side_len);
	set_string(10, parsed.exec_type, parsed.exec_type_len);
	set_string(11, parsed.ord_status, parsed.ord_status_len);
	set_double(12, parsed.price, parsed.price_len, "Price");
	set_double(13, parsed.order_qty, parsed.order_qty_len, "OrderQty");
	set_double(14, parsed.cum_qty, parsed.cum_qty_len, "CumQty");
	set_double(15, parsed.leaves_qty, parsed.leaves_qty_len, "LeavesQty");
	set_double(16, parsed.last_px, parsed.last_px_len, "LastPx");
	set_double(17, parsed.last_qty, parsed.last_qty_len, "LastQty");
	set_string(18, parsed.text, parsed.text_len);
}

void FixColumnWriter::WriteTagsMap(const ParsedFixMessage &parsed) {
	auto out_idx = GetOutputIdx(19);
	if (out_idx == DConstants::INVALID_INDEX) {
		return;
	}

	if (!gstate.needs_tags || parsed.other_tags.empty()) {
		output.data[out_idx].SetValue(row_idx, Value());
		return;
	}

	// Build MAP(INTEGER, VARCHAR) from other_tags
	vector<Value> map_entries;
	for (const auto &entry : parsed.other_tags) {
		child_list_t<Value> map_struct;
		map_struct.push_back(make_pair("key", Value::INTEGER(entry.first)));
		string tag_value_str(entry.second.data, entry.second.len);
		map_struct.push_back(make_pair("value", Value(tag_value_str)));
		map_entries.push_back(Value::STRUCT(map_struct));
	}

	auto map_type = LogicalType::MAP(LogicalType::INTEGER, LogicalType::VARCHAR);
	auto child_type = ListType::GetChildType(map_type);
	output.data[out_idx].SetValue(row_idx, Value::MAP(child_type, map_entries));
}

void FixColumnWriter::WriteGroupsMap(const ParsedFixMessage &parsed) {
	auto out_idx = GetOutputIdx(20);
	if (out_idx == DConstants::INVALID_INDEX) {
		return;
	}

	Value groups_value = FixGroupParser::ParseGroups(parsed, *bind_data.dictionary, gstate.needs_groups);
	output.data[out_idx].SetValue(row_idx, groups_value);
}

void FixColumnWriter::WriteMetadata(const string &raw_line) {
	// raw_message column (21)
	auto out_idx = GetOutputIdx(21);
	if (out_idx != DConstants::INVALID_INDEX) {
		output.data[out_idx].SetValue(row_idx, Value(raw_line));
	}

	// parse_error column (22)
	out_idx = GetOutputIdx(22);
	if (out_idx != DConstants::INVALID_INDEX) {
		if (conversion_errors.empty()) {
			output.data[out_idx].SetValue(row_idx, Value());
		} else {
			string combined_error;
			for (size_t i = 0; i < conversion_errors.size(); i++) {
				if (i > 0) {
					combined_error += "; ";
				}
				combined_error += conversion_errors[i];
			}
			output.data[out_idx].SetValue(row_idx, Value(combined_error));
		}
	}
}

void FixColumnWriter::WriteCustomTags(const ParsedFixMessage &parsed) {
	for (size_t i = 0; i < bind_data.custom_tags.size(); i++) {
		const auto &tag_pair = bind_data.custom_tags[i];
		const string &tag_name = tag_pair.first;
		int tag_num = tag_pair.second;
		auto out_idx = GetOutputIdx(23 + i);

		if (out_idx == DConstants::INVALID_INDEX) {
			continue;
		}

		// Find value in hot tags first, then other_tags
		const char *value_ptr = nullptr;
		size_t value_len = 0;

		// Check if this is a hot tag - using centralized FixHotTags constants
		switch (tag_num) {
		case duckdb::FixHotTags::MSG_TYPE:
			value_ptr = parsed.msg_type;
			value_len = parsed.msg_type_len;
			break;
		case duckdb::FixHotTags::SENDER_COMP_ID:
			value_ptr = parsed.sender_comp_id;
			value_len = parsed.sender_comp_id_len;
			break;
		case duckdb::FixHotTags::TARGET_COMP_ID:
			value_ptr = parsed.target_comp_id;
			value_len = parsed.target_comp_id_len;
			break;
		case duckdb::FixHotTags::MSG_SEQ_NUM:
			value_ptr = parsed.msg_seq_num;
			value_len = parsed.msg_seq_num_len;
			break;
		case duckdb::FixHotTags::SENDING_TIME:
			value_ptr = parsed.sending_time;
			value_len = parsed.sending_time_len;
			break;
		case duckdb::FixHotTags::CL_ORD_ID:
			value_ptr = parsed.cl_ord_id;
			value_len = parsed.cl_ord_id_len;
			break;
		case duckdb::FixHotTags::ORDER_ID:
			value_ptr = parsed.order_id;
			value_len = parsed.order_id_len;
			break;
		case duckdb::FixHotTags::EXEC_ID:
			value_ptr = parsed.exec_id;
			value_len = parsed.exec_id_len;
			break;
		case duckdb::FixHotTags::SYMBOL:
			value_ptr = parsed.symbol;
			value_len = parsed.symbol_len;
			break;
		case duckdb::FixHotTags::SIDE:
			value_ptr = parsed.side;
			value_len = parsed.side_len;
			break;
		case duckdb::FixHotTags::EXEC_TYPE:
			value_ptr = parsed.exec_type;
			value_len = parsed.exec_type_len;
			break;
		case duckdb::FixHotTags::ORD_STATUS:
			value_ptr = parsed.ord_status;
			value_len = parsed.ord_status_len;
			break;
		case duckdb::FixHotTags::PRICE:
			value_ptr = parsed.price;
			value_len = parsed.price_len;
			break;
		case duckdb::FixHotTags::ORDER_QTY:
			value_ptr = parsed.order_qty;
			value_len = parsed.order_qty_len;
			break;
		case duckdb::FixHotTags::CUM_QTY:
			value_ptr = parsed.cum_qty;
			value_len = parsed.cum_qty_len;
			break;
		case duckdb::FixHotTags::LEAVES_QTY:
			value_ptr = parsed.leaves_qty;
			value_len = parsed.leaves_qty_len;
			break;
		case duckdb::FixHotTags::LAST_PX:
			value_ptr = parsed.last_px;
			value_len = parsed.last_px_len;
			break;
		case duckdb::FixHotTags::LAST_QTY:
			value_ptr = parsed.last_qty;
			value_len = parsed.last_qty_len;
			break;
		case duckdb::FixHotTags::TEXT:
			value_ptr = parsed.text;
			value_len = parsed.text_len;
			break;
		default:
			// Not a hot tag, check other_tags
			auto it = parsed.other_tags.find(tag_num);
			if (it != parsed.other_tags.end()) {
				value_ptr = it->second.data;
				value_len = it->second.len;
			}
			break;
		}

		SetStringField(output.data[out_idx], row_idx, value_ptr, value_len);
	}
}

// Scan function - called repeatedly to fill DataChunks
static void ReadFixScan(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind_data = data_p.bind_data->Cast<ReadFixBindData>();
	auto &gstate = data_p.global_state->Cast<ReadFixGlobalState>();
	auto &lstate = data_p.local_state->Cast<ReadFixLocalState>();

	idx_t output_idx = 0;

	// Open file if needed using FixFileReader
	if (!lstate.file_reader.IsOpen()) {
		auto &fs = FileSystem::GetFileSystem(context);
		if (!lstate.file_reader.OpenNextFile(fs, bind_data.files, gstate.file_index, gstate.lock)) {
			// No more files
			output.SetCardinality(0);
			return;
		}
	}

	// Read and parse lines using FixFileReader
	string line;
	while (output_idx < STANDARD_VECTOR_SIZE) {
		if (!lstate.file_reader.ReadLine(line)) {
			// End of file, close and try next file
			lstate.file_reader.Close();

			auto &fs = FileSystem::GetFileSystem(context);
			if (!lstate.file_reader.OpenNextFile(fs, bind_data.files, gstate.file_index, gstate.lock)) {
				// No more files
				break;
			}
			continue;
		}

		// Skip empty lines
		if (line.empty()) {
			continue;
		}

		// Parse FIX message
		ParsedFixMessage parsed;
		FixTokenizer::Parse(line.c_str(), line.size(), parsed, bind_data.delimiter);

		// Initialize error collection
		vector<string> conversion_errors;
		if (!parsed.parse_error.empty()) {
			conversion_errors.push_back(parsed.parse_error);
		}

		// Use FixColumnWriter helper to write all columns
		FixColumnWriter writer(output, output_idx, bind_data, gstate, conversion_errors);
		writer.WriteHotTags(parsed);
		writer.WriteTagsMap(parsed);
		writer.WriteGroupsMap(parsed);
		writer.WriteMetadata(line);
		writer.WriteCustomTags(parsed);

		output_idx++;
	}

	output.SetCardinality(output_idx);
}

// Get the table function definition
TableFunction ReadFixFunction::GetFunction() {
	TableFunction func("read_fix", {LogicalType(LogicalTypeId::VARCHAR)}, ReadFixScan, ReadFixBind, ReadFixInitGlobal,
	                   ReadFixInitLocal);
	func.name = "read_fix";

	// Phase 7.5: Enable projection pushdown
	func.projection_pushdown = true;

	// Phase 7.5: Custom tag parameters
	func.named_parameters["rtags"] = LogicalType::LIST(LogicalType(LogicalTypeId::VARCHAR));  // Tag names
	func.named_parameters["tagIds"] = LogicalType::LIST(LogicalType(LogicalTypeId::INTEGER)); // Tag numbers

	// Phase 7.7: Delimiter parameter
	func.named_parameters["delimiter"] = LogicalType(LogicalTypeId::VARCHAR);

	// Phase 7.8: Dictionary parameter
	func.named_parameters["dictionary"] = LogicalType(LogicalTypeId::VARCHAR);

	return func;
}

} // namespace duckdb
