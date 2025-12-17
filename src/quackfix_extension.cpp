#define DUCKDB_EXTENSION_MAIN

#include "quackfix_extension.hpp"
#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/function/scalar_function.hpp"
#include <duckdb/parser/parsed_data/create_scalar_function_info.hpp>
#include "table_function/read_fix_function.hpp"
#include "table_function/dictionary_functions.hpp"

// OpenSSL linked through vcpkg
#include <openssl/opensslv.h>

namespace duckdb {

inline void QuackfixScalarFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &name_vector = args.data[0];
	UnaryExecutor::Execute<string_t, string_t>(name_vector, result, args.size(), [&](string_t name) {
		return StringVector::AddString(result, "Quackfix " + name.GetString() + " 🐥");
	});
}

inline void QuackfixOpenSSLVersionScalarFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &name_vector = args.data[0];
	UnaryExecutor::Execute<string_t, string_t>(name_vector, result, args.size(), [&](string_t name) {
		return StringVector::AddString(result, "Quackfix " + name.GetString() + ", my linked OpenSSL version is " +
		                                           OPENSSL_VERSION_TEXT);
	});
}

static void LoadInternal(ExtensionLoader &loader) {
	// Register a scalar function
	auto quackfix_scalar_function =
	    ScalarFunction("quackfix", {LogicalType::VARCHAR}, LogicalType::VARCHAR, QuackfixScalarFun);
	loader.RegisterFunction(quackfix_scalar_function);

	// Register another scalar function
	auto quackfix_openssl_version_scalar_function = ScalarFunction(
	    "quackfix_openssl_version", {LogicalType::VARCHAR}, LogicalType::VARCHAR, QuackfixOpenSSLVersionScalarFun);
	loader.RegisterFunction(quackfix_openssl_version_scalar_function);

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
