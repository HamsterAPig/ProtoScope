#pragma once

struct sqlite3;
namespace protoscope::storage {
void registerQueryFunctions(sqlite3* db);
}
