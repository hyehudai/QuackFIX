#include <iostream>
#include <cassert>
#include <string>

#include "dictionary/xml_loader.hpp"
#include "dictionary/fix_dictionary.hpp"

// The build system injects this at compile time.
// Example: "/home/hanany/QuackFIX"
#ifndef QUACKFIX_ROOT
#error "QUACKFIX_ROOT not defined! Add target_compile_definitions."
#endif

static std::string path(const std::string &p) {
	return std::string(QUACKFIX_ROOT) + "/" + p;
}

int main() {
	std::cout << "Running QuackFIX Dictionary Tests...\n";

	// ------------------------------------------------------------
	// Test 1: Load base FIX44 dictionary
	// ------------------------------------------------------------
	std::string fix44_path = path("dialects/FIX44.xml");
	FixDictionary dict;

	try {
		dict = FixDictionaryLoader::LoadBase(fix44_path);
	} catch (const std::exception &ex) {
		std::cerr << "ERROR: Failed to load FIX44.xml: " << ex.what() << "\n";
		return 1;
	}

	// Basic field tests
	assert(dict.fields.count(35) == 1); // MsgType
	assert(dict.fields.at(35).name == "MsgType");
	assert(dict.name_to_tag.at("MsgType") == 35);

	// Check a couple more fields
	assert(dict.fields.count(49) == 1); // SenderCompID
	assert(dict.fields.count(56) == 1); // TargetCompID

	// ------------------------------------------------------------
	// Test 2: Message definitions
	// ------------------------------------------------------------
	assert(dict.messages.count("D") == 1); // NewOrderSingle

	const FixMessageDef &nos = dict.messages.at("D");
	assert(nos.name == "NewOrderSingle");
	assert(!nos.required_fields.empty());

	// Check required fields include ClOrdID (tag 11)
	bool found_clordid = false;
	for (int tag : nos.required_fields) {
		if (tag == 11)
			found_clordid = true;
	}
	assert(found_clordid);

	// ------------------------------------------------------------
	// Test 3: Repeating groups
	// Example: FIX44 NewOrderSingle has a PartyID group (tag 453)
	// ------------------------------------------------------------
	if (nos.groups.count(453)) {
		const FixGroupDef &party_group = nos.groups.at(453);
		assert(party_group.count_tag == 453);
		assert(!party_group.field_tags.empty()); // PartyID, PartyRole, etc.
	}

	// ------------------------------------------------------------
	// Test 4: Overlay dictionary (optional test)
	// Only run if example_dialect.xml exists
	// ------------------------------------------------------------
	std::string overlay_path = path("dialects/example_dialect.xml");
	if (FILE *fp = fopen(overlay_path.c_str(), "r")) {
		fclose(fp);
		std::cout << "Overlay test: example_dialect.xml found. Testing overlay...\n";

		try {
			FixDictionaryLoader::ApplyOverlay(dict, overlay_path);
		} catch (const std::exception &ex) {
			std::cerr << "ERROR: Failed to load overlay: " << ex.what() << "\n";
			return 1;
		}

		// Check overlay: should add custom field 25036 if example dialect does that
		if (dict.fields.count(25036)) {
			std::cout << "Overlay field 25036 loaded successfully.\n";
			assert(dict.fields.at(25036).name == "ResponseMode");
		}
	} else {
		std::cout << "Overlay test: no example_dialect.xml found — skipping overlay tests.\n";
	}

	std::cout << "All dictionary tests passed!" << std::endl;
	return 0;
}
