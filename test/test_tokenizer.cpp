#include <iostream>
#include <cassert>
#include <string>
#include <cstring>

#include "parser/fix_tokenizer.hpp"
#include "parser/fix_message.hpp"

// Helper to compare string with pointer/length
bool str_eq(const char *ptr, size_t len, const char *expected) {
	if (ptr == nullptr)
		return false;
	return len == strlen(expected) && strncmp(ptr, expected, len) == 0;
}

void test_basic_parsing() {
	std::cout << "Test: Basic FIX message parsing..." << std::endl;

	// Test with pipe delimiter for readability
	std::string msg = "8=FIX.4.4|9=100|35=D|49=SENDER|56=TARGET|34=1|52=20231215-10:30:00|11=ORDER123|55=AAPL|54=1|38="
	                  "100|44=150.50|10=000";

	ParsedFixMessage parsed;
	bool success = FixTokenizer::Parse(msg.c_str(), msg.size(), parsed, '|');

	assert(success && "Parse should succeed");
	assert(parsed.msg_type != nullptr && "MsgType should be set");
	assert(str_eq(parsed.msg_type, parsed.msg_type_len, "D") && "MsgType should be D");
	assert(str_eq(parsed.sender_comp_id, parsed.sender_comp_id_len, "SENDER"));
	assert(str_eq(parsed.target_comp_id, parsed.target_comp_id_len, "TARGET"));
	assert(str_eq(parsed.msg_seq_num, parsed.msg_seq_num_len, "1"));
	assert(str_eq(parsed.cl_ord_id, parsed.cl_ord_id_len, "ORDER123"));
	assert(str_eq(parsed.symbol, parsed.symbol_len, "AAPL"));
	assert(str_eq(parsed.side, parsed.side_len, "1"));
	assert(str_eq(parsed.order_qty, parsed.order_qty_len, "100"));
	assert(str_eq(parsed.price, parsed.price_len, "150.50"));

	std::cout << "  ✓ Basic parsing works correctly" << std::endl;
}

void test_execution_report() {
	std::cout << "Test: Execution report parsing..." << std::endl;

	std::string msg = "8=FIX.4.4|35=8|49=TARGET|56=SENDER|34=2|37=EXEC001|11=ORDER123|17=TRADE001|150=F|39=2|55=AAPL|"
	                  "54=1|38=100|14=100|151=0|31=150.50|32=100";

	ParsedFixMessage parsed;
	bool success = FixTokenizer::Parse(msg.c_str(), msg.size(), parsed, '|');

	assert(success && "Parse should succeed");
	assert(str_eq(parsed.msg_type, parsed.msg_type_len, "8") && "MsgType should be 8 (ExecutionReport)");
	assert(str_eq(parsed.order_id, parsed.order_id_len, "EXEC001"));
	assert(str_eq(parsed.exec_id, parsed.exec_id_len, "TRADE001"));
	assert(str_eq(parsed.exec_type, parsed.exec_type_len, "F"));
	assert(str_eq(parsed.ord_status, parsed.ord_status_len, "2"));
	assert(str_eq(parsed.cum_qty, parsed.cum_qty_len, "100"));
	assert(str_eq(parsed.leaves_qty, parsed.leaves_qty_len, "0"));
	assert(str_eq(parsed.last_px, parsed.last_px_len, "150.50"));
	assert(str_eq(parsed.last_qty, parsed.last_qty_len, "100"));

	std::cout << "  ✓ Execution report parsing works correctly" << std::endl;
}

void test_other_tags() {
	std::cout << "Test: Non-hot tags stored in other_tags..." << std::endl;

	std::string msg = "35=D|49=SENDER|8=FIX.4.4|9=100|21=1|40=2|59=0|60=20231215-10:30:00|10=000";

	ParsedFixMessage parsed;
	bool success = FixTokenizer::Parse(msg.c_str(), msg.size(), parsed, '|');

	assert(success && "Parse should succeed");
	assert(parsed.other_tags.count(8) == 1 && "Tag 8 should be in other_tags");
	assert(str_eq(parsed.other_tags[8].data, parsed.other_tags[8].len, "FIX.4.4"));
	assert(parsed.other_tags.count(9) == 1 && "Tag 9 should be in other_tags");
	assert(str_eq(parsed.other_tags[9].data, parsed.other_tags[9].len, "100"));
	assert(parsed.other_tags.count(21) == 1 && "Tag 21 should be in other_tags");
	assert(parsed.other_tags.count(40) == 1 && "Tag 40 should be in other_tags");
	assert(parsed.other_tags.count(59) == 1 && "Tag 59 should be in other_tags");
	assert(parsed.other_tags.count(60) == 1 && "Tag 60 should be in other_tags");
	assert(parsed.other_tags.count(10) == 1 && "Tag 10 (checksum) should be in other_tags");

	std::cout << "  ✓ Non-hot tags correctly stored" << std::endl;
}

void test_soh_delimiter() {
	std::cout << "Test: SOH delimiter parsing..." << std::endl;

	// Build message with SOH delimiter
	std::string msg = "35=D\x01"
	                  "49=SENDER\x01"
	                  "56=TARGET\x01"
	                  "11=ORDER123\x01"
	                  "55=MSFT";

	ParsedFixMessage parsed;
	bool success = FixTokenizer::Parse(msg.c_str(), msg.size(), parsed, '\x01');

	assert(success && "Parse should succeed with SOH delimiter");
	assert(str_eq(parsed.msg_type, parsed.msg_type_len, "D"));
	assert(str_eq(parsed.sender_comp_id, parsed.sender_comp_id_len, "SENDER"));
	assert(str_eq(parsed.target_comp_id, parsed.target_comp_id_len, "TARGET"));
	assert(str_eq(parsed.cl_ord_id, parsed.cl_ord_id_len, "ORDER123"));
	assert(str_eq(parsed.symbol, parsed.symbol_len, "MSFT"));

	std::cout << "  ✓ SOH delimiter parsing works correctly" << std::endl;
}

void test_missing_msgtype() {
	std::cout << "Test: Missing MsgType error..." << std::endl;

	std::string msg = "49=SENDER|56=TARGET|11=ORDER123";

	ParsedFixMessage parsed;
	bool success = FixTokenizer::Parse(msg.c_str(), msg.size(), parsed, '|');

	assert(!success && "Parse should fail without MsgType");
	assert(!parsed.parse_error.empty() && "Parse error should be set");
	assert(parsed.parse_error.find("MsgType") != std::string::npos && "Error should mention MsgType");

	std::cout << "  ✓ Missing MsgType correctly detected" << std::endl;
}

void test_invalid_format() {
	std::cout << "Test: Invalid tag format error..." << std::endl;

	std::string msg = "35=D|49SENDER|56=TARGET"; // Missing = in tag 49

	ParsedFixMessage parsed;
	bool success = FixTokenizer::Parse(msg.c_str(), msg.size(), parsed, '|');

	assert(!success && "Parse should fail with invalid format");
	assert(!parsed.parse_error.empty() && "Parse error should be set");

	std::cout << "  ✓ Invalid format correctly detected" << std::endl;
}

void test_empty_message() {
	std::cout << "Test: Empty message error..." << std::endl;

	std::string msg = "";

	ParsedFixMessage parsed;
	bool success = FixTokenizer::Parse(msg.c_str(), msg.size(), parsed, '|');

	assert(!success && "Parse should fail with empty message");
	assert(!parsed.parse_error.empty() && "Parse error should be set");

	std::cout << "  ✓ Empty message correctly detected" << std::endl;
}

void test_raw_message_stored() {
	std::cout << "Test: Raw message is stored..." << std::endl;

	std::string msg = "35=D|49=SENDER|56=TARGET|55=AAPL";

	ParsedFixMessage parsed;
	bool success = FixTokenizer::Parse(msg.c_str(), msg.size(), parsed, '|');

	assert(success && "Parse should succeed");
	assert(parsed.raw_message != nullptr && "Raw message should be stored");
	assert(parsed.raw_message_len == msg.size() && "Raw message length should match");
	assert(strncmp(parsed.raw_message, msg.c_str(), msg.size()) == 0 && "Raw message content should match");

	std::cout << "  ✓ Raw message correctly stored" << std::endl;
}

int main() {
	std::cout << "Running QuackFIX Tokenizer Tests...\n" << std::endl;

	try {
		test_basic_parsing();
		test_execution_report();
		test_other_tags();
		test_soh_delimiter();
		test_missing_msgtype();
		test_invalid_format();
		test_empty_message();
		test_raw_message_stored();

		std::cout << "\n✅ All tokenizer tests passed!" << std::endl;
		return 0;
	} catch (const std::exception &ex) {
		std::cerr << "\n❌ Test failed with exception: " << ex.what() << std::endl;
		return 1;
	}
}
