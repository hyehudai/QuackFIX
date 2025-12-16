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
#include "parser/fix_tokenizer.hpp"
#include "parser/fix_message.hpp"
#include <sstream>

namespace duckdb {

// Bind data - configuration for the table function
struct ReadFixBindData : public TableFunctionData {
    vector<string> files;
    shared_ptr<FixDictionary> dictionary;
    
    // Phase 7.5: Custom tag support (rtags + tagIds parameters)
    vector<pair<string, int>> custom_tags;  // {tag_name, tag_number}
    
    // Phase 7.7: Delimiter parameter
    char delimiter = '|';  // Default to pipe
    
    ReadFixBindData() {}
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
    
    ReadFixGlobalState() : file_index(0), needs_tags(true), needs_groups(true) {}
    
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

// Local state - per-thread state
struct ReadFixLocalState : public LocalTableFunctionState {
    unique_ptr<FileHandle> file_handle;
    string current_file;
    idx_t line_number;
    bool file_done;
    string buffer;
    idx_t buffer_offset;
    
    ReadFixLocalState() : line_number(0), file_done(false), buffer_offset(0) {}
};

// Bind function - called once at query planning time
static unique_ptr<FunctionData> ReadFixBind(ClientContext &context, TableFunctionBindInput &input,
                                             vector<LogicalType> &return_types, vector<string> &names) {
    auto result = make_uniq<ReadFixBindData>();
    
    // Get file path parameter
    if (input.inputs.size() < 1) {
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
    
    // Phase 7.8: Parse dictionary parameter (default: dialects/FIX44.xml)
    string dict_path = "dialects/FIX44.xml";
    if (input.named_parameters.find("dictionary") != input.named_parameters.end()) {
        dict_path = StringValue::Get(input.named_parameters.at("dictionary"));
    }
    
    // Load FIX dictionary for group parsing and custom tag validation
    // Phase 7.8: Use FileSystem API to support S3, HTTP, etc.
    try {
        auto dict = FixDictionaryLoader::LoadBase(context, dict_path);
        result->dictionary = make_shared_ptr<FixDictionary>(std::move(dict));
    } catch (const std::exception& e) {
        throw BinderException("Failed to load FIX dictionary from '%s': %s", dict_path.c_str(), e.what());
    }
    
    // Phase 7.7: Parse delimiter parameter
    if (input.named_parameters.find("delimiter") != input.named_parameters.end()) {
        string delim_str = StringValue::Get(input.named_parameters.at("delimiter"));
        if (delim_str.empty()) {
            throw BinderException("delimiter cannot be empty");
        } else if (delim_str.size() == 1) {
            result->delimiter = delim_str[0];
        } else if (delim_str == "\\x01") {
            result->delimiter = '\x01';  // SOH
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
                throw BinderException("Invalid tag name in rtags: '%s'. Tag not found in FIX dictionary.", tag_name.c_str());
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
    return_types.emplace_back(LogicalType::VARCHAR);
    
    names.emplace_back("SenderCompID");
    return_types.emplace_back(LogicalType::VARCHAR);
    
    names.emplace_back("TargetCompID");
    return_types.emplace_back(LogicalType::VARCHAR);
    
    names.emplace_back("MsgSeqNum");
    return_types.emplace_back(LogicalType::BIGINT);  // Numeric
    
    names.emplace_back("SendingTime");
    return_types.emplace_back(LogicalType::TIMESTAMP);  // Timestamp with milliseconds
    
    names.emplace_back("ClOrdID");
    return_types.emplace_back(LogicalType::VARCHAR);
    
    names.emplace_back("OrderID");
    return_types.emplace_back(LogicalType::VARCHAR);
    
    names.emplace_back("ExecID");
    return_types.emplace_back(LogicalType::VARCHAR);
    
    names.emplace_back("Symbol");
    return_types.emplace_back(LogicalType::VARCHAR);
    
    names.emplace_back("Side");
    return_types.emplace_back(LogicalType::VARCHAR);
    
    names.emplace_back("ExecType");
    return_types.emplace_back(LogicalType::VARCHAR);
    
    names.emplace_back("OrdStatus");
    return_types.emplace_back(LogicalType::VARCHAR);
    
    names.emplace_back("Price");
    return_types.emplace_back(LogicalType::DOUBLE);  // Numeric
    
    names.emplace_back("OrderQty");
    return_types.emplace_back(LogicalType::DOUBLE);  // Numeric
    
    names.emplace_back("CumQty");
    return_types.emplace_back(LogicalType::DOUBLE);  // Numeric
    
    names.emplace_back("LeavesQty");
    return_types.emplace_back(LogicalType::DOUBLE);  // Numeric
    
    names.emplace_back("LastPx");
    return_types.emplace_back(LogicalType::DOUBLE);  // Numeric
    
    names.emplace_back("LastQty");
    return_types.emplace_back(LogicalType::DOUBLE);  // Numeric
    
    names.emplace_back("Text");
    return_types.emplace_back(LogicalType::VARCHAR);
    
    // Phase 5: Non-hot tags
    names.emplace_back("tags");
    return_types.emplace_back(LogicalType::MAP(LogicalType::INTEGER, LogicalType::VARCHAR));
    
    // Phase 5: Repeating groups
    names.emplace_back("groups");
    return_types.emplace_back(LogicalType::MAP(LogicalType::INTEGER, 
                               LogicalType::LIST(LogicalType::MAP(LogicalType::INTEGER, LogicalType::VARCHAR))));
    
    names.emplace_back("raw_message");
    return_types.emplace_back(LogicalType::VARCHAR);
    
    names.emplace_back("parse_error");
    return_types.emplace_back(LogicalType::VARCHAR);
    
    // Phase 7.5: Add custom tag columns (after standard columns)
    for (const auto &[tag_name, tag_num] : result->custom_tags) {
        names.emplace_back(tag_name);
        return_types.emplace_back(LogicalType::VARCHAR);
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

// Scan function - called repeatedly to fill DataChunks
static void ReadFixScan(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
    auto &bind_data = data_p.bind_data->Cast<ReadFixBindData>();
    auto &gstate = data_p.global_state->Cast<ReadFixGlobalState>();
    auto &lstate = data_p.local_state->Cast<ReadFixLocalState>();
    
    idx_t output_idx = 0;
    
    // Open file if needed
    if (!lstate.file_handle) {
        lock_guard<mutex> lock(gstate.lock);
        
        if (gstate.file_index >= bind_data.files.size()) {
            // No more files
            output.SetCardinality(0);
            return;
        }
        
        lstate.current_file = bind_data.files[gstate.file_index];
        gstate.file_index++;
        
        // Use DuckDB FileSystem API to open file (supports S3, HTTP, etc.)
        auto &fs = FileSystem::GetFileSystem(context);
        lstate.file_handle = fs.OpenFile(lstate.current_file, FileFlags::FILE_FLAGS_READ);
        lstate.line_number = 0;
        lstate.file_done = false;
        lstate.buffer.clear();
        lstate.buffer_offset = 0;
    }
    
    // Read and parse lines
    string line;
    while (output_idx < STANDARD_VECTOR_SIZE && !lstate.file_done) {
        // Read a line from the file using FileHandle
        line.clear();
        bool found_line = false;
        
        while (!lstate.file_done) {
            // Check if we need to read more data into buffer
            if (lstate.buffer_offset >= lstate.buffer.size()) {
                // Read next chunk
                constexpr idx_t BUFFER_SIZE = 8192;
                lstate.buffer.resize(BUFFER_SIZE);
                idx_t bytes_read = lstate.file_handle->Read((void*)lstate.buffer.data(), BUFFER_SIZE);
                
                if (bytes_read == 0) {
                    // End of file
                    lstate.file_done = true;
                    if (!line.empty()) {
                        found_line = true;
                    }
                    break;
                }
                
                lstate.buffer.resize(bytes_read);
                lstate.buffer_offset = 0;
            }
            
            // Find newline in buffer
            size_t newline_pos = lstate.buffer.find('\n', lstate.buffer_offset);
            if (newline_pos != string::npos) {
                // Found newline
                line.append(lstate.buffer, lstate.buffer_offset, newline_pos - lstate.buffer_offset);
                lstate.buffer_offset = newline_pos + 1;
                found_line = true;
                break;
            } else {
                // No newline in current buffer, append all remaining data
                line.append(lstate.buffer, lstate.buffer_offset, lstate.buffer.size() - lstate.buffer_offset);
                lstate.buffer_offset = lstate.buffer.size();
            }
        }
        
        if (!found_line) {
            // No more lines, close file
            lstate.file_handle.reset();
            break;
        }
        
        // Remove trailing carriage return if present (for Windows line endings)
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        
        lstate.line_number++;
        
        // Skip empty lines
        if (line.empty()) {
            continue;
        }
        
        // Parse FIX message
        ParsedFixMessage parsed;
        bool success = FixTokenizer::Parse(line.c_str(), line.size(), parsed, bind_data.delimiter);
        
        // Accumulate conversion errors
        vector<string> conversion_errors;
        if (!parsed.parse_error.empty()) {
            conversion_errors.push_back(parsed.parse_error);
        }
        
        // Helper lambda to set string value from parsed field
        auto set_string_field = [&](idx_t col_idx, const char* ptr, size_t len) {
            if (ptr != nullptr && len > 0) {
                output.data[col_idx].SetValue(output_idx, Value(string(ptr, len)));
            } else {
                output.data[col_idx].SetValue(output_idx, Value());
            }
        };
        
        // Helper lambda to parse and set int64 value (lenient)
        auto set_int64_field = [&](idx_t col_idx, const char* ptr, size_t len, const char* field_name) {
            if (ptr != nullptr && len > 0) {
                try {
                    string str_val(ptr, len);
                    size_t pos;
                    int64_t val = std::stoll(str_val, &pos);
                    if (pos != str_val.length()) {
                        throw std::invalid_argument("extra characters");
                    }
                    output.data[col_idx].SetValue(output_idx, Value::BIGINT(val));
                } catch (const std::exception& e) {
                    output.data[col_idx].SetValue(output_idx, Value());
                    conversion_errors.push_back(string("Invalid ") + field_name + ": '" + string(ptr, len) + "'");
                }
            } else {
                output.data[col_idx].SetValue(output_idx, Value());
            }
        };
        
        // Helper lambda to parse and set double value (lenient)
        auto set_double_field = [&](idx_t col_idx, const char* ptr, size_t len, const char* field_name) {
            if (ptr != nullptr && len > 0) {
                try {
                    string str_val(ptr, len);
                    size_t pos;
                    double val = std::stod(str_val, &pos);
                    if (pos != str_val.length()) {
                        throw std::invalid_argument("extra characters");
                    }
                    output.data[col_idx].SetValue(output_idx, Value::DOUBLE(val));
                } catch (const std::exception& e) {
                    output.data[col_idx].SetValue(output_idx, Value());
                    conversion_errors.push_back(string("Invalid ") + field_name + ": '" + string(ptr, len) + "'");
                }
            } else {
                output.data[col_idx].SetValue(output_idx, Value());
            }
        };
        
        // Helper lambda to parse FIX timestamp (lenient)
        // Format: YYYYMMDD-HH:MM:SS[.sss]
        // Example: 20231215-10:30:00 or 20231215-10:30:00.123
        auto set_timestamp_field = [&](idx_t col_idx, const char* ptr, size_t len, const char* field_name) {
            if (ptr != nullptr && len >= 17) {  // Minimum: YYYYMMDD-HH:MM:SS
                try {
                    // Parse YYYYMMDD-HH:MM:SS[.sss]
                    auto parse_2digits = [](const char* p) -> int {
                        return (p[0] - '0') * 10 + (p[1] - '0');
                    };
                    auto parse_4digits = [](const char* p) -> int {
                        return (p[0] - '0') * 1000 + (p[1] - '0') * 100 + (p[2] - '0') * 10 + (p[3] - '0');
                    };
                    
                    int year = parse_4digits(ptr);      // YYYY
                    int month = parse_2digits(ptr+4);   // MM
                    int day = parse_2digits(ptr+6);     // DD
                    // ptr[8] is '-'
                    int hour = parse_2digits(ptr+9);    // HH
                    // ptr[11] is ':'
                    int minute = parse_2digits(ptr+12); // MM
                    // ptr[14] is ':'
                    int second = parse_2digits(ptr+15); // SS
                    
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
                    timestamp_t ts = Timestamp::FromDatetime(date, time);
                    
                    output.data[col_idx].SetValue(output_idx, Value::TIMESTAMP(ts));
                } catch (const std::exception& e) {
                    output.data[col_idx].SetValue(output_idx, Value());
                    conversion_errors.push_back(string("Invalid ") + field_name + ": '" + string(ptr, len) + "'");
                }
            } else {
                output.data[col_idx].SetValue(output_idx, Value());
            }
        };
        
        // Phase 7.5: Map schema column indices to output column indices
        // When projection pushdown is active, output only has the columns being scanned
        // We need to write to the correct position in the output DataChunk
        auto get_output_idx = [&](idx_t schema_col_idx) -> idx_t {
            // Find the position of this schema column in column_indexes
            for (idx_t i = 0; i < gstate.column_indexes.size(); i++) {
                if (gstate.column_indexes[i].GetPrimaryIndex() == schema_col_idx) {
                    return i;
                }
            }
            // Column not in output - skip it
            return DConstants::INVALID_INDEX;
        };
        
        // Output all columns (23 columns total)
        // We use get_output_idx to map schema column index to output column index
        
        // Column 0: MsgType
        auto out_idx = get_output_idx(0);
        if (out_idx != DConstants::INVALID_INDEX) {
            set_string_field(out_idx, parsed.msg_type, parsed.msg_type_len);
        }
        
        // Column 1: SenderCompID
        out_idx = get_output_idx(1);
        if (out_idx != DConstants::INVALID_INDEX) {
            set_string_field(out_idx, parsed.sender_comp_id, parsed.sender_comp_id_len);
        }
        
        // Column 2: TargetCompID
        out_idx = get_output_idx(2);
        if (out_idx != DConstants::INVALID_INDEX) {
            set_string_field(out_idx, parsed.target_comp_id, parsed.target_comp_id_len);
        }
        
        // Column 3: MsgSeqNum (BIGINT)
        out_idx = get_output_idx(3);
        if (out_idx != DConstants::INVALID_INDEX) {
            set_int64_field(out_idx, parsed.msg_seq_num, parsed.msg_seq_num_len, "MsgSeqNum");
        }
        
        // Column 4: SendingTime (TIMESTAMP)
        out_idx = get_output_idx(4);
        if (out_idx != DConstants::INVALID_INDEX) {
            set_timestamp_field(out_idx, parsed.sending_time, parsed.sending_time_len, "SendingTime");
        }
        
        // Column 5: ClOrdID
        out_idx = get_output_idx(5);
        if (out_idx != DConstants::INVALID_INDEX) {
            set_string_field(out_idx, parsed.cl_ord_id, parsed.cl_ord_id_len);
        }
        
        // Column 6: OrderID
        out_idx = get_output_idx(6);
        if (out_idx != DConstants::INVALID_INDEX) {
            set_string_field(out_idx, parsed.order_id, parsed.order_id_len);
        }
        
        // Column 7: ExecID
        out_idx = get_output_idx(7);
        if (out_idx != DConstants::INVALID_INDEX) {
            set_string_field(out_idx, parsed.exec_id, parsed.exec_id_len);
        }
        
        // Column 8: Symbol
        out_idx = get_output_idx(8);
        if (out_idx != DConstants::INVALID_INDEX) {
            set_string_field(out_idx, parsed.symbol, parsed.symbol_len);
        }
        
        // Column 9: Side
        out_idx = get_output_idx(9);
        if (out_idx != DConstants::INVALID_INDEX) {
            set_string_field(out_idx, parsed.side, parsed.side_len);
        }
        
        // Column 10: ExecType
        out_idx = get_output_idx(10);
        if (out_idx != DConstants::INVALID_INDEX) {
            set_string_field(out_idx, parsed.exec_type, parsed.exec_type_len);
        }
        
        // Column 11: OrdStatus
        out_idx = get_output_idx(11);
        if (out_idx != DConstants::INVALID_INDEX) {
            set_string_field(out_idx, parsed.ord_status, parsed.ord_status_len);
        }
        
        // Column 12: Price (DOUBLE)
        out_idx = get_output_idx(12);
        if (out_idx != DConstants::INVALID_INDEX) {
            set_double_field(out_idx, parsed.price, parsed.price_len, "Price");
        }
        
        // Column 13: OrderQty (DOUBLE)
        out_idx = get_output_idx(13);
        if (out_idx != DConstants::INVALID_INDEX) {
            set_double_field(out_idx, parsed.order_qty, parsed.order_qty_len, "OrderQty");
        }
        
        // Column 14: CumQty (DOUBLE)
        out_idx = get_output_idx(14);
        if (out_idx != DConstants::INVALID_INDEX) {
            set_double_field(out_idx, parsed.cum_qty, parsed.cum_qty_len, "CumQty");
        }
        
        // Column 15: LeavesQty (DOUBLE)
        out_idx = get_output_idx(15);
        if (out_idx != DConstants::INVALID_INDEX) {
            set_double_field(out_idx, parsed.leaves_qty, parsed.leaves_qty_len, "LeavesQty");
        }
        
        // Column 16: LastPx (DOUBLE)
        out_idx = get_output_idx(16);
        if (out_idx != DConstants::INVALID_INDEX) {
            set_double_field(out_idx, parsed.last_px, parsed.last_px_len, "LastPx");
        }
        
        // Column 17: LastQty (DOUBLE)
        out_idx = get_output_idx(17);
        if (out_idx != DConstants::INVALID_INDEX) {
            set_double_field(out_idx, parsed.last_qty, parsed.last_qty_len, "LastQty");
        }
        
        // Column 18: Text
        out_idx = get_output_idx(18);
        if (out_idx != DConstants::INVALID_INDEX) {
            set_string_field(out_idx, parsed.text, parsed.text_len);
        }
        
        // Column 19: tags (non-hot tags from other_tags map)
        // Phase 7.5: Skip tags processing if not needed
        out_idx = get_output_idx(19);
        if (out_idx != DConstants::INVALID_INDEX) {
            if (!gstate.needs_tags) {
                output.data[out_idx].SetValue(output_idx, Value());  // NULL - tags not requested
            } else if (parsed.other_tags.empty()) {
                output.data[out_idx].SetValue(output_idx, Value());  // NULL if no other tags
            } else {
                // Build MAP(INTEGER, VARCHAR) from other_tags
                vector<Value> map_entries;
                for (const auto& entry : parsed.other_tags) {
                    child_list_t<Value> map_struct;
                    map_struct.push_back(make_pair("key", Value::INTEGER(entry.first)));
                    // Convert TagValue to string
                    string tag_value_str(entry.second.data, entry.second.len);
                    map_struct.push_back(make_pair("value", Value(tag_value_str)));
                    map_entries.push_back(Value::STRUCT(map_struct));
                }
                
                // Get the child type from the MAP type (STRUCT with key/value)
                auto map_type = LogicalType::MAP(LogicalType::INTEGER, LogicalType::VARCHAR);
                auto child_type = ListType::GetChildType(map_type);
                output.data[out_idx].SetValue(output_idx, Value::MAP(child_type, map_entries));
            }
        }
        
        // Column 20: groups (dictionary-driven repeating groups)
        // Phase 7.5: Skip groups processing if not needed (major performance gain!)
        out_idx = get_output_idx(20);
        if (out_idx != DConstants::INVALID_INDEX) {
            if (!gstate.needs_groups) {
                output.data[out_idx].SetValue(output_idx, Value());  // NULL - groups not requested
            } else if (!bind_data.dictionary || parsed.all_tags_ordered.empty() || 
                       parsed.msg_type == nullptr || parsed.msg_type_len == 0) {
                output.data[out_idx].SetValue(output_idx, Value());  // NULL if no dictionary or no message type
            } else {
                // Look up message type in dictionary
                string msg_type_str(parsed.msg_type, parsed.msg_type_len);
                auto msg_it = bind_data.dictionary->messages.find(msg_type_str);
                
                if (msg_it == bind_data.dictionary->messages.end()) {
                    // Message type not in dictionary - no groups to parse
                    output.data[out_idx].SetValue(output_idx, Value());
            } else {
                // Parse repeating groups using dictionary definitions
                vector<Value> outer_map_entries;
                const auto& message_def = msg_it->second;
                
                // Iterate through all groups defined for this message type
                for (const auto& [count_tag, group_def] : message_def.groups) {
                    // Check if this group exists in the message
                    auto tag_it = parsed.other_tags.find(count_tag);
                    if (tag_it == parsed.other_tags.end()) {
                        continue;  // Group not present in message
                    }
                    
                    // Parse group count
                    string count_str(tag_it->second.data, tag_it->second.len);
                    int group_count = 0;
                    try {
                        group_count = std::stoi(count_str);
                    } catch (...) {
                        continue;  // Invalid count
                    }
                    
                    if (group_count <= 0 || group_count > 100) {
                        continue;  // Sanity check
                    }
                    
                    // Get field tags from dictionary (std::vector from dictionary)
                    const std::vector<int>& group_field_tags = group_def.field_tags;
                    if (group_field_tags.empty()) {
                        continue;  // No fields defined for this group
                    }
                    
                    // Find the position of the count tag in ordered list
                    size_t count_tag_pos = 0;
                    bool found_count_tag = false;
                    for (size_t i = 0; i < parsed.all_tags_ordered.size(); i++) {
                        if (parsed.all_tags_ordered[i].first == count_tag) {
                            count_tag_pos = i;
                            found_count_tag = true;
                            break;
                        }
                    }
                    
                    if (!found_count_tag) {
                        continue;  // Count tag not found in ordered list
                    }
                    
                    // Parse group instances from ordered tags starting after count tag
                    vector<Value> group_instances;
                    size_t pos = count_tag_pos + 1;
                    
                    for (int instance = 0; instance < group_count && pos < parsed.all_tags_ordered.size(); instance++) {
                        // Parse one group instance
                        vector<Value> instance_map_entries;
                        
                        // Collect tags that belong to this group instance
                        while (pos < parsed.all_tags_ordered.size()) {
                            int tag = parsed.all_tags_ordered[pos].first;
                            
                            // Check if this tag belongs to the current group (using dictionary)
                            bool is_group_field = false;
                            for (int gf : group_field_tags) {
                                if (tag == gf) {
                                    is_group_field = true;
                                    break;
                                }
                            }
                            
                            if (!is_group_field) {
                                // Not a group field - either another group starts or non-group tag
                                break;
                            }
                            
                            // Add this tag to the instance
                            auto& tag_value = parsed.all_tags_ordered[pos].second;
                            child_list_t<Value> map_entry;
                            map_entry.push_back(make_pair("key", Value::INTEGER(tag)));
                            map_entry.push_back(make_pair("value", Value(string(tag_value.data, tag_value.len))));
                            instance_map_entries.push_back(Value::STRUCT(map_entry));
                            
                            pos++;
                            
                            // Check if we've seen the first field again (marks next instance)
                            if (pos < parsed.all_tags_ordered.size() && 
                                parsed.all_tags_ordered[pos].first == group_field_tags[0]) {
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
                    output.data[out_idx].SetValue(output_idx, Value());  // NULL if no groups found
                } else {
                    auto outer_map_type = LogicalType::MAP(LogicalType::INTEGER, 
                                           LogicalType::LIST(LogicalType::MAP(LogicalType::INTEGER, LogicalType::VARCHAR)));
                    auto outer_child_type = ListType::GetChildType(outer_map_type);
                    output.data[out_idx].SetValue(output_idx, Value::MAP(outer_child_type, outer_map_entries));
                }
            }
            }
        }
        
        // Column 21: raw_message
        out_idx = get_output_idx(21);
        if (out_idx != DConstants::INVALID_INDEX) {
            output.data[out_idx].SetValue(output_idx, Value(line));
        }
        
        // Column 22: parse_error (accumulated)
        out_idx = get_output_idx(22);
        if (out_idx != DConstants::INVALID_INDEX) {
            if (conversion_errors.empty()) {
                output.data[out_idx].SetValue(output_idx, Value());
            } else {
                // Join all errors with "; "
                string combined_error;
                for (size_t i = 0; i < conversion_errors.size(); i++) {
                    if (i > 0) combined_error += "; ";
                    combined_error += conversion_errors[i];
                }
                output.data[out_idx].SetValue(output_idx, Value(combined_error));
            }
        }
        
        // Phase 7.5: Custom tag columns (columns 23+)
        // Extract values from hot tags or other_tags
        for (size_t i = 0; i < bind_data.custom_tags.size(); i++) {
            const auto &[tag_name, tag_num] = bind_data.custom_tags[i];
            out_idx = get_output_idx(23 + i);
            
            if (out_idx != DConstants::INVALID_INDEX) {
                // Find value in hot tags first, then other_tags
                const char* value_ptr = nullptr;
                size_t value_len = 0;
                
                // Check if this is a hot tag (35, 49, 56, 34, 52, 11, 37, 17, 55, 54, 150, 39, 44, 38, 14, 151, 31, 32, 58)
                switch (tag_num) {
                    case 35: value_ptr = parsed.msg_type; value_len = parsed.msg_type_len; break;
                    case 49: value_ptr = parsed.sender_comp_id; value_len = parsed.sender_comp_id_len; break;
                    case 56: value_ptr = parsed.target_comp_id; value_len = parsed.target_comp_id_len; break;
                    case 34: value_ptr = parsed.msg_seq_num; value_len = parsed.msg_seq_num_len; break;
                    case 52: value_ptr = parsed.sending_time; value_len = parsed.sending_time_len; break;
                    case 11: value_ptr = parsed.cl_ord_id; value_len = parsed.cl_ord_id_len; break;
                    case 37: value_ptr = parsed.order_id; value_len = parsed.order_id_len; break;
                    case 17: value_ptr = parsed.exec_id; value_len = parsed.exec_id_len; break;
                    case 55: value_ptr = parsed.symbol; value_len = parsed.symbol_len; break;
                    case 54: value_ptr = parsed.side; value_len = parsed.side_len; break;
                    case 150: value_ptr = parsed.exec_type; value_len = parsed.exec_type_len; break;
                    case 39: value_ptr = parsed.ord_status; value_len = parsed.ord_status_len; break;
                    case 44: value_ptr = parsed.price; value_len = parsed.price_len; break;
                    case 38: value_ptr = parsed.order_qty; value_len = parsed.order_qty_len; break;
                    case 14: value_ptr = parsed.cum_qty; value_len = parsed.cum_qty_len; break;
                    case 151: value_ptr = parsed.leaves_qty; value_len = parsed.leaves_qty_len; break;
                    case 31: value_ptr = parsed.last_px; value_len = parsed.last_px_len; break;
                    case 32: value_ptr = parsed.last_qty; value_len = parsed.last_qty_len; break;
                    case 58: value_ptr = parsed.text; value_len = parsed.text_len; break;
                    default:
                        // Not a hot tag, check other_tags
                        auto it = parsed.other_tags.find(tag_num);
                        if (it != parsed.other_tags.end()) {
                            value_ptr = it->second.data;
                            value_len = it->second.len;
                        }
                        break;
                }
                
                set_string_field(out_idx, value_ptr, value_len);
            }
        }
        
        output_idx++;
    }
    
    output.SetCardinality(output_idx);
}

// Get the table function definition
TableFunction ReadFixFunction::GetFunction() {
    TableFunction func("read_fix", {LogicalType::VARCHAR}, ReadFixScan, ReadFixBind, ReadFixInitGlobal, ReadFixInitLocal);
    func.name = "read_fix";
    
    // Phase 7.5: Enable projection pushdown
    func.projection_pushdown = true;
    
    // Phase 7.5: Custom tag parameters
    func.named_parameters["rtags"] = LogicalType::LIST(LogicalType::VARCHAR);   // Tag names
    func.named_parameters["tagIds"] = LogicalType::LIST(LogicalType::INTEGER);  // Tag numbers
    
    // Phase 7.7: Delimiter parameter
    func.named_parameters["delimiter"] = LogicalType::VARCHAR;
    
    // Phase 7.8: Dictionary parameter
    func.named_parameters["dictionary"] = LogicalType::VARCHAR;
    
    return func;
}

} // namespace duckdb
