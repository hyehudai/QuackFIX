#pragma once

#include "fix_dictionary.hpp"
#include "tinyxml2.h"
#include <string>

// Forward declaration
namespace duckdb {
class ClientContext;
}

class FixDictionaryLoader {
public:
	// Phase 7.8: Updated to support DuckDB FileSystem (S3, HTTP, etc.)
	static FixDictionary LoadBase(duckdb::ClientContext &context, const std::string &path);
	static FixDictionary LoadFromString(const std::string &xml_content);
	static void ApplyOverlay(duckdb::ClientContext &context, FixDictionary &dict, const std::string &path);

private:
	static void LoadFields(FixDictionary &dict, tinyxml2::XMLElement *fields_root);
	static void LoadComponents(FixDictionary &dict, tinyxml2::XMLElement *components_root);
	static void LoadMessages(FixDictionary &dict, tinyxml2::XMLElement *messages_root);
	static FixGroupDef LoadGroup(FixDictionary &dict, tinyxml2::XMLElement *group);
	static void ExpandComponent(FixDictionary &dict, FixMessageDef &msg, tinyxml2::XMLElement *comp_ref);
};
