#pragma once
#include <string>
#include <unordered_map>
#include <vector>
#include <memory>

// -------------------------------
// ENUM DEFINITIONS
// -------------------------------
struct FixEnum {
	std::string enum_value;
	std::string description;
};

// -------------------------------
// FIELD DEFINITIONS
// -------------------------------
struct FixFieldDef {
	int tag;
	std::string name;
	std::string type; // STRING, INT, PRICE, QTY, CHAR, CHECKSUM, etc.
	std::vector<FixEnum> enums;
};

// -------------------------------
// GROUP DEFINITIONS (Repeating groups)
// -------------------------------
struct FixGroupDef {
	int count_tag;                                           // e.g. 268 = NoMDEntries
	std::vector<int> field_tags;                             // tags within group
	std::unordered_map<int, std::shared_ptr<FixGroupDef>> subgroups; // nested groups
};

// -------------------------------
// MESSAGE DEFINITIONS
// -------------------------------
struct FixMessageDef {
	std::string name;     // "NewOrderSingle"
	std::string msg_type; // "D"

	std::vector<int> required_fields;
	std::vector<int> optional_fields;

	std::unordered_map<int, std::shared_ptr<FixGroupDef>> groups;
};

// -------------------------------
// COMPONENT DEFINITIONS
// -------------------------------
struct FixComponentDef {
	std::string name;
	std::vector<int> field_tags;
	std::unordered_map<int, std::shared_ptr<FixGroupDef>> groups;
};

// -------------------------------
// DICTIONARY ROOT
// -------------------------------
struct FixDictionary {
	std::unordered_map<int, FixFieldDef> fields;                 // tag → definition
	std::unordered_map<std::string, FixMessageDef> messages;     // MsgType → message def
	std::unordered_map<std::string, FixComponentDef> components; // name → component def

	// Reverse lookup: name → tag
	std::unordered_map<std::string, int> name_to_tag;
};
