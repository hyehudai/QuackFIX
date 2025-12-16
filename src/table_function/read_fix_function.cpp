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
    
    ReadFixBindData() {}
};

// Global state - shared across all threads
struct ReadFixGlobalState : public GlobalTableFunctionState {
    idx_t file_index;
    mutex lock;
    
    ReadFixGlobalState() : file_index(0) {}
    
    idx_t MaxThreads() const override {
        return 1; // Single-threaded for Phase 3
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
    
    // Load FIX dictionary for group parsing
    try {
        auto dict = FixDictionaryLoader::LoadBase("dialects/FIX44.xml");
        result->dictionary = make_shared_ptr<FixDictionary>(std::move(dict));
    } catch (const std::exception& e) {
        // Dictionary loading failed - groups will return NULL
        result->dictionary.reset();
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
    
    return std::move(result);
}

// InitGlobal - initialize global state
static unique_ptr<GlobalTableFunctionState> ReadFixInitGlobal(ClientContext &context, TableFunctionInitInput &input) {
    auto result = make_uniq<ReadFixGlobalState>();
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
        bool success = FixTokenizer::Parse(line.c_str(), line.size(), parsed, '|');
        
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
        
        // Output all hot tag columns (21 columns total)
        idx_t col = 0;
        
        // Column 0: MsgType
        set_string_field(col++, parsed.msg_type, parsed.msg_type_len);
        
        // Column 1: SenderCompID
        set_string_field(col++, parsed.sender_comp_id, parsed.sender_comp_id_len);
        
        // Column 2: TargetCompID
        set_string_field(col++, parsed.target_comp_id, parsed.target_comp_id_len);
        
        // Column 3: MsgSeqNum (BIGINT)
        set_int64_field(col++, parsed.msg_seq_num, parsed.msg_seq_num_len, "MsgSeqNum");
        
        // Column 4: SendingTime (TIMESTAMP)
        set_timestamp_field(col++, parsed.sending_time, parsed.sending_time_len, "SendingTime");
        
        // Column 5: ClOrdID
        set_string_field(col++, parsed.cl_ord_id, parsed.cl_ord_id_len);
        
        // Column 6: OrderID
        set_string_field(col++, parsed.order_id, parsed.order_id_len);
        
        // Column 7: ExecID
        set_string_field(col++, parsed.exec_id, parsed.exec_id_len);
        
        // Column 8: Symbol
        set_string_field(col++, parsed.symbol, parsed.symbol_len);
        
        // Column 9: Side
        set_string_field(col++, parsed.side, parsed.side_len);
        
        // Column 10: ExecType
        set_string_field(col++, parsed.exec_type, parsed.exec_type_len);
        
        // Column 11: OrdStatus
        set_string_field(col++, parsed.ord_status, parsed.ord_status_len);
        
        // Column 12: Price (DOUBLE)
        set_double_field(col++, parsed.price, parsed.price_len, "Price");
        
        // Column 13: OrderQty (DOUBLE)
        set_double_field(col++, parsed.order_qty, parsed.order_qty_len, "OrderQty");
        
        // Column 14: CumQty (DOUBLE)
        set_double_field(col++, parsed.cum_qty, parsed.cum_qty_len, "CumQty");
        
        // Column 15: LeavesQty (DOUBLE)
        set_double_field(col++, parsed.leaves_qty, parsed.leaves_qty_len, "LeavesQty");
        
        // Column 16: LastPx (DOUBLE)
        set_double_field(col++, parsed.last_px, parsed.last_px_len, "LastPx");
        
        // Column 17: LastQty (DOUBLE)
        set_double_field(col++, parsed.last_qty, parsed.last_qty_len, "LastQty");
        
        // Column 18: Text
        set_string_field(col++, parsed.text, parsed.text_len);
        
        // Column 19: tags (non-hot tags from other_tags map)
        if (parsed.other_tags.empty()) {
            output.data[col++].SetValue(output_idx, Value());  // NULL if no other tags
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
            output.data[col++].SetValue(output_idx, Value::MAP(child_type, map_entries));
        }
        
        // Column 20: groups (dictionary-driven repeating groups)
        if (!bind_data.dictionary || parsed.all_tags_ordered.empty() || 
            parsed.msg_type == nullptr || parsed.msg_type_len == 0) {
            output.data[col++].SetValue(output_idx, Value());  // NULL if no dictionary or no message type
        } else {
            // Look up message type in dictionary
            string msg_type_str(parsed.msg_type, parsed.msg_type_len);
            auto msg_it = bind_data.dictionary->messages.find(msg_type_str);
            
            if (msg_it == bind_data.dictionary->messages.end()) {
                // Message type not in dictionary - no groups to parse
                output.data[col++].SetValue(output_idx, Value());
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
                    output.data[col++].SetValue(output_idx, Value());  // NULL if no groups found
                } else {
                    auto outer_map_type = LogicalType::MAP(LogicalType::INTEGER, 
                                           LogicalType::LIST(LogicalType::MAP(LogicalType::INTEGER, LogicalType::VARCHAR)));
                    auto outer_child_type = ListType::GetChildType(outer_map_type);
                    output.data[col++].SetValue(output_idx, Value::MAP(outer_child_type, outer_map_entries));
                }
            }
        }
        
        // Column 21: raw_message
        output.data[col++].SetValue(output_idx, Value(line));
        
        // Column 22: parse_error (accumulated)
        if (conversion_errors.empty()) {
            output.data[col++].SetValue(output_idx, Value());
        } else {
            // Join all errors with "; "
            string combined_error;
            for (size_t i = 0; i < conversion_errors.size(); i++) {
                if (i > 0) combined_error += "; ";
                combined_error += conversion_errors[i];
            }
            output.data[col++].SetValue(output_idx, Value(combined_error));
        }
        
        output_idx++;
    }
    
    output.SetCardinality(output_idx);
}

// Get the table function definition
TableFunction ReadFixFunction::GetFunction() {
    TableFunction func("read_fix", {LogicalType::VARCHAR}, ReadFixScan, ReadFixBind, ReadFixInitGlobal, ReadFixInitLocal);
    func.name = "read_fix";
    return func;
}

} // namespace duckdb
