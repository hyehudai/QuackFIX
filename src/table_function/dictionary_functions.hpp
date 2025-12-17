#pragma once
#include "duckdb/function/table_function.hpp"

namespace duckdb {

class FixFieldsFunction {
public:
	static TableFunction GetFunction();
};

class FixMessageFieldsFunction {
public:
	static TableFunction GetFunction();
};

class FixGroupsFunction {
public:
	static TableFunction GetFunction();
};

} // namespace duckdb
