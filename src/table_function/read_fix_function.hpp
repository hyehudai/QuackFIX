#pragma once

#include "duckdb.hpp"
#include "duckdb/function/table_function.hpp"

namespace duckdb {

struct ReadFixFunction {
    static TableFunction GetFunction();
};

} // namespace duckdb
