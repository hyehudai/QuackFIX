#define DUCKDB_EXTENSION_MAIN

#include "quackfix_extension.hpp"
#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/function/scalar_function.hpp"
#include <duckdb/parser/parsed_data/create_scalar_function_info.hpp>
#include "table_function/read_fix_function.hpp"
#include "table_function/dictionary_functions.hpp"

namespace duckdb {

static void LoadInternal(ExtensionLoader &loader) {
	// Register the read_fix table function
	auto read_fix_function = ReadFixFunction::GetFunction();
	loader.RegisterFunction(read_fix_function);

	// Register dictionary exploration functions
	auto fix_fields_function = FixFieldsFunction::GetFunction();
	loader.RegisterFunction(fix_fields_function);

	auto fix_message_fields_function = FixMessageFieldsFunction::GetFunction();
	loader.RegisterFunction(fix_message_fields_function);

	auto fix_groups_function = FixGroupsFunction::GetFunction();
	loader.RegisterFunction(fix_groups_function);
}

void QuackfixExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}
std::string QuackfixExtension::Name() {
	return "quackfix";
}

std::string QuackfixExtension::Version() const {
#ifdef EXT_VERSION_QUACKFIX
	return EXT_VERSION_QUACKFIX;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(quackfix, loader) {
	duckdb::LoadInternal(loader);
}
}
