#define CATCH_CONFIG_MAIN
#include "catch.hpp"
#include "sqlite3.h"
#include <string>
#include <thread>

using namespace std;

static int concatenate_results(void* arg, int ncols, char** vals, char** colnames) {
	auto &results = *((vector<vector<string>> *) arg);
	if (results.size() == 0) {
		results.resize(ncols);
	}
	for(int i = 0; i < ncols; i++) {
		results[i].push_back(vals[i] ? vals[i] : "");
	}
	return SQLITE_OK;
}

// C++ wrapper class for the C wrapper API that wraps our C++ API, because why not
class SQLiteDBWrapper {
public:
	SQLiteDBWrapper() :
		db(nullptr) {
	}
	~SQLiteDBWrapper() {
		if (db) {
			sqlite3_close(db);
		}
	}

	sqlite3 *db;
	vector<vector<string>> results;
public:
	int Open(string filename) {
		return sqlite3_open(filename.c_str(), &db) == SQLITE_OK;
	}

	string GetErrorMessage() {
		auto err = sqlite3_errmsg(db);
		return err ? string(err) : string();
	}

	bool Execute(string query) {
		results.clear();
		char *errmsg = nullptr;
		int rc = sqlite3_exec(db, query.c_str(), concatenate_results, &results, &errmsg);
		if (errmsg) {
			sqlite3_free(errmsg);
		}
		return rc == SQLITE_OK;
	}

	void PrintResult() {
		for(size_t row_idx = 0; row_idx < results[0].size(); row_idx++) {
			for(size_t col_idx = 0; col_idx < results.size(); col_idx++) {
				printf("%s|", results[col_idx][row_idx].c_str());
			}
			printf("\n");
		}
	}

	bool CheckColumn(size_t column, vector<string> expected_data) {
		if (column >= results.size()) {
			fprintf(stderr, "Column index is out of range!\n");
			PrintResult();
			return false;
		}
		if (results[column].size() != expected_data.size()) {
			fprintf(stderr, "Row counts do not match!\n");
			PrintResult();
			return false;
		}
		for(size_t i = 0; i < expected_data.size(); i++) {
			if (expected_data[i] != results[column][i]) {
				fprintf(stderr, "Value does not match: expected \"%s\" but got \"%s\"\n", expected_data[i].c_str(), results[column][i].c_str());
				return false;
			}
		}
		return true;
	}
};

class SQLiteStmtWrapper {
public:
	SQLiteStmtWrapper() :
		stmt(nullptr) {}
	~SQLiteStmtWrapper() {
		Finalize();
	}

	sqlite3_stmt *stmt;
	string error_message;

	int Prepare(sqlite3 *db, const char *zSql, int nByte, const char **pzTail) {
		Finalize();
		return sqlite3_prepare_v2(db, zSql, nByte, &stmt, pzTail);
	}

	void Finalize() {
		if (stmt) {
			sqlite3_finalize(stmt);
			stmt = nullptr;
		}
	}
};

TEST_CASE("Basic sqlite wrapper usage", "[sqlite3wrapper]" ) {
	SQLiteDBWrapper db;

	// open an in-memory db
	REQUIRE(db.Open(":memory:"));

	// standard selection
	REQUIRE(db.Execute("SELECT 42;"));
	REQUIRE(db.CheckColumn(0, {"42"}));

	// simple statements
	REQUIRE(db.Execute("CREATE TABLE test(i INTEGER)"));
	REQUIRE(db.Execute("INSERT INTO test VALUES (1), (2), (3)"));
	REQUIRE(db.Execute("SELECT SUM(t1.i) FROM test t1, test t2, test t3;"));
	REQUIRE(db.CheckColumn(0, {"54"}));

	REQUIRE(db.Execute("DELETE FROM test WHERE i=2"));
	REQUIRE(db.Execute("UPDATE test SET i=i+1"));
	REQUIRE(db.Execute("SELECT * FROM test ORDER BY 1;"));
	REQUIRE(db.CheckColumn(0, {"2", "4"}));

	// test different types
#ifndef SQLITE_TEST
	REQUIRE(db.Execute("SELECT CAST('1992-01-01' AS DATE), 3, 'hello world', CAST('1992-01-01 00:00:00' AS TIMESTAMP);"));
	REQUIRE(db.CheckColumn(0, {"1992-01-01"}));
	REQUIRE(db.CheckColumn(1, {"3"}));
	REQUIRE(db.CheckColumn(2, {"hello world"}));
	REQUIRE(db.CheckColumn(3, {"1992-01-01 00:00:00"}));
#endif

	// handle errors
	// syntax error
	REQUIRE(!db.Execute("SELEC 42"));
	// catalog error
	REQUIRE(!db.Execute("SELECT * FROM nonexistant_tbl"));
}

TEST_CASE("Basic prepared statement usage", "[sqlite3wrapper]" ) {
	SQLiteDBWrapper db;
	SQLiteStmtWrapper stmt;

	// open an in-memory db
	REQUIRE(db.Open(":memory:"));
	REQUIRE(db.Execute("CREATE TABLE test(i INTEGER, j BIGINT, k DATE, l VARCHAR)"));
#ifndef SQLITE_TEST
	// sqlite3_prepare_v2 errors
	// nullptr for db/stmt, note: normal sqlite segfaults here
	REQUIRE(sqlite3_prepare_v2(nullptr, "INSERT INTO test VALUES ($1, $2, $3, $4)", -1, nullptr, nullptr) == SQLITE_MISUSE);
	REQUIRE(sqlite3_prepare_v2(db.db, "INSERT INTO test VALUES ($1, $2, $3, $4)", -1, nullptr, nullptr) == SQLITE_MISUSE);
#endif
	// prepared statement
	REQUIRE(stmt.Prepare(db.db, "INSERT INTO test VALUES ($1, $2, $3, $4)", -1, nullptr) == SQLITE_OK);

	// test for parameter count, names and indexes
	REQUIRE(sqlite3_bind_parameter_count(nullptr) == 0);
	REQUIRE(sqlite3_bind_parameter_count(stmt.stmt) == 4);
	for(int i = 1; i < 5; i++) {
		REQUIRE(sqlite3_bind_parameter_name(nullptr, i) == nullptr);
		REQUIRE(sqlite3_bind_parameter_index(nullptr, nullptr) == 0);
		REQUIRE(sqlite3_bind_parameter_index(stmt.stmt, nullptr) == 0);
		REQUIRE(sqlite3_bind_parameter_name(stmt.stmt, i) != nullptr);
		REQUIRE(sqlite3_bind_parameter_name(stmt.stmt, i) == string("$") + to_string(i));
		REQUIRE(sqlite3_bind_parameter_index(stmt.stmt, sqlite3_bind_parameter_name(stmt.stmt, i)) == i);
	}
	REQUIRE(sqlite3_bind_parameter_name(stmt.stmt, 0) == nullptr);
	REQUIRE(sqlite3_bind_parameter_name(stmt.stmt, 5) == nullptr);

#ifndef SQLITE_TEST
	// this segfaults in SQLITE
	REQUIRE(sqlite3_clear_bindings(nullptr) == SQLITE_MISUSE);
#endif
	REQUIRE(sqlite3_clear_bindings(stmt.stmt) == SQLITE_OK);
	REQUIRE(sqlite3_clear_bindings(stmt.stmt) == SQLITE_OK);
	// test for binding parameters
	// incorrect bindings: nullptr as statement, wrong type and out of range binding
	REQUIRE(sqlite3_bind_int(nullptr, 1, 1) == SQLITE_MISUSE);
	REQUIRE(sqlite3_bind_int(stmt.stmt, 0, 1) == SQLITE_RANGE);
	REQUIRE(sqlite3_bind_int(stmt.stmt, 5, 1) == SQLITE_RANGE);

	// we can bind the incorrect type just fine
	// error will only be thrown on execution
	REQUIRE(sqlite3_bind_text(stmt.stmt, 1, "hello world", -1, nullptr) == SQLITE_OK);
	REQUIRE(sqlite3_bind_int(stmt.stmt, 1, 1) == SQLITE_OK);
	// we can rebind the same parameter
	REQUIRE(sqlite3_bind_int(stmt.stmt, 1, 2) == SQLITE_OK);
	REQUIRE(sqlite3_bind_int64(stmt.stmt, 2, 1000) == SQLITE_OK);
	REQUIRE(sqlite3_bind_text(stmt.stmt, 3, "1992-01-01", -1, nullptr) == SQLITE_OK);
	REQUIRE(sqlite3_bind_text(stmt.stmt, 4, "hello world", -1, nullptr) == SQLITE_OK);

	REQUIRE(sqlite3_step(nullptr) == SQLITE_MISUSE);
	REQUIRE(sqlite3_step(stmt.stmt) == SQLITE_DONE);

	// reset the statement
	REQUIRE(sqlite3_reset(nullptr) == SQLITE_OK);
	REQUIRE(sqlite3_reset(stmt.stmt) == SQLITE_OK);
	// we can reset multiple times
	REQUIRE(sqlite3_reset(stmt.stmt) == SQLITE_OK);

	REQUIRE(sqlite3_bind_null(stmt.stmt, 1) == SQLITE_OK);
	REQUIRE(sqlite3_bind_null(stmt.stmt, 2) == SQLITE_OK);
	REQUIRE(sqlite3_bind_null(stmt.stmt, 3) == SQLITE_OK);
	REQUIRE(sqlite3_bind_null(stmt.stmt, 4) == SQLITE_OK);

	// we can step multiple times
	REQUIRE(sqlite3_step(stmt.stmt) == SQLITE_DONE);
	REQUIRE(sqlite3_step(stmt.stmt) == SQLITE_DONE);
	REQUIRE(sqlite3_reset(stmt.stmt) == SQLITE_OK);
	// after a reset we still have our bound values
	REQUIRE(sqlite3_step(stmt.stmt) == SQLITE_DONE);
	// clearing the bindings results in us not having any values though
	REQUIRE(sqlite3_clear_bindings(stmt.stmt) == SQLITE_OK);
	REQUIRE(sqlite3_step(stmt.stmt) == SQLITE_DONE);

	REQUIRE(db.Execute("SELECT * FROM test ORDER BY 1"));
	REQUIRE(db.CheckColumn(0, {"", "", "", "", "2"}));
	REQUIRE(db.CheckColumn(1, {"", "", "", "", "1000"}));
	REQUIRE(db.CheckColumn(2, {"", "", "", "", "1992-01-01"}));
	REQUIRE(db.CheckColumn(3, {"", "", "", "", "hello world"}));

	REQUIRE(sqlite3_finalize(nullptr) == SQLITE_OK);

	// first prepare the statement again
	REQUIRE(stmt.Prepare(db.db, "SELECT * FROM test WHERE i=CAST($1 AS INTEGER)", -1, nullptr) == SQLITE_OK);
	// bind a non-integer here
	REQUIRE(sqlite3_bind_text(stmt.stmt, 1, "hello", -1, nullptr) == SQLITE_OK);
#ifndef SQLITE_TEST
	REQUIRE(sqlite3_step(stmt.stmt) == SQLITE_ERROR);
	REQUIRE(sqlite3_step(stmt.stmt) == SQLITE_ERROR);
	REQUIRE(sqlite3_step(stmt.stmt) == SQLITE_ERROR);
	// need to be prepare aggain
	REQUIRE(stmt.Prepare(db.db, "SELECT * FROM test WHERE i=CAST($1 AS INTEGER)", -1, nullptr) == SQLITE_OK);
	REQUIRE(sqlite3_bind_text(stmt.stmt, 1, "2", -1, nullptr) == SQLITE_OK);
	REQUIRE(sqlite3_step(stmt.stmt) == SQLITE_ROW);
#else
	// sqlite allows string to int casts ("hello" becomes 0)
	REQUIRE(sqlite3_step(stmt.stmt) == SQLITE_DONE);
	REQUIRE(sqlite3_step(stmt.stmt) == SQLITE_DONE);
#endif

	// rebind and call again
	// need to reset first
	REQUIRE(sqlite3_bind_text(stmt.stmt, 1, "1", -1, nullptr) == SQLITE_MISUSE);
	REQUIRE(sqlite3_reset(stmt.stmt) == SQLITE_OK);

	REQUIRE(sqlite3_bind_text(stmt.stmt, 1, "2", -1, nullptr) == SQLITE_OK);
	// repeatedly call sqlite3_step on a SELECT statement
	REQUIRE(sqlite3_step(stmt.stmt) == SQLITE_ROW);
	// verify the results
	REQUIRE(string((char*) sqlite3_column_text(stmt.stmt, 0)) == string("2"));
	REQUIRE(sqlite3_column_int(stmt.stmt, 0) == 2);
	REQUIRE(sqlite3_column_int64(stmt.stmt, 0) == 2);
	REQUIRE(sqlite3_column_double(stmt.stmt, 0) == 2);
	REQUIRE(string((char*) sqlite3_column_text(stmt.stmt, 1)) == string("1000"));
	REQUIRE(string((char*) sqlite3_column_text(stmt.stmt, 2)) == string("1992-01-01"));
	REQUIRE(string((char*) sqlite3_column_text(stmt.stmt, 3)) == string("hello world"));
	REQUIRE(sqlite3_column_int(stmt.stmt, 3) == 0);
	REQUIRE(sqlite3_column_int64(stmt.stmt, 3) == 0);
	REQUIRE(sqlite3_column_double(stmt.stmt, 3) == 0);
	REQUIRE(sqlite3_column_text(stmt.stmt, -1) == nullptr);
	REQUIRE(sqlite3_column_text(stmt.stmt, 10) == nullptr);

	REQUIRE(sqlite3_step(stmt.stmt) == SQLITE_DONE);
	// no data in the current row
	REQUIRE(sqlite3_column_int(stmt.stmt, 0) == 0);
	REQUIRE(sqlite3_column_int(nullptr, 0) == 0);
	// the query resets again after SQLITE_DONE
	REQUIRE(sqlite3_step(stmt.stmt) == SQLITE_ROW);
	REQUIRE(sqlite3_step(stmt.stmt) == SQLITE_DONE);

	// sqlite bind and errors
	REQUIRE(stmt.Prepare(db.db, "SELECT * FROM non_existant_table", -1, nullptr) == SQLITE_ERROR);
	REQUIRE(stmt.stmt == nullptr);

	// sqlite3 prepare leftovers
	// empty statement
	const char *leftover;
	REQUIRE(stmt.Prepare(db.db, "", -1, &leftover) == SQLITE_OK);
	REQUIRE(leftover != nullptr);
	REQUIRE(string(leftover) == "");
	// leftover comment
	REQUIRE(stmt.Prepare(db.db, "SELECT 42; --hello\nSELECT 3", -1, &leftover) == SQLITE_OK);
	REQUIRE(leftover != nullptr);
	REQUIRE(string(leftover) == " --hello\nSELECT 3");
	// leftover extra statement
	REQUIRE(stmt.Prepare(db.db, "SELECT 42--hello;\n, 3; SELECT 17", -1, &leftover) == SQLITE_OK);
	REQUIRE(leftover != nullptr);
	REQUIRE(string(leftover) == " SELECT 17");
	// no query
	REQUIRE(stmt.Prepare(db.db, nullptr, -1, &leftover) == SQLITE_MISUSE);

	// sqlite3 prepare nByte
	// any negative value can be used, not just -1
	REQUIRE(stmt.Prepare(db.db, "SELECT 42", -1000, &leftover) == SQLITE_OK);
	REQUIRE(sqlite3_step(stmt.stmt) == SQLITE_ROW);
	REQUIRE(sqlite3_column_int(stmt.stmt, 0) == 42);
	REQUIRE(sqlite3_step(stmt.stmt) == SQLITE_DONE);
	// we can use nByte to skip reading part of string (in this case, skip WHERE 1=0)
	REQUIRE(stmt.Prepare(db.db, "SELECT 42 WHERE 1=0", 9, &leftover) == SQLITE_OK);
	REQUIRE(sqlite3_step(stmt.stmt) == SQLITE_ROW);
	REQUIRE(sqlite3_column_int(stmt.stmt, 0) == 42);
	REQUIRE(sqlite3_step(stmt.stmt) == SQLITE_DONE);
	// using too large nByte?
	REQUIRE(stmt.Prepare(db.db, "SELECT 42 WHERE 1=0", 19, &leftover) == SQLITE_OK);
	REQUIRE(sqlite3_step(stmt.stmt) == SQLITE_DONE);
}


static void sqlite3_interrupt_fast(SQLiteDBWrapper *db, bool *success) {
	*success = db->Execute("SELECT SUM(i1.i) FROM integers i1, integers i2, integers i3, integers i4, integers i5");
}

TEST_CASE("Test sqlite3_interrupt", "[sqlite3wrapper]" ) {
	SQLiteDBWrapper db;
	bool success;

	// open an in-memory db
	REQUIRE(db.Open(":memory:"));
	REQUIRE(db.Execute("CREATE TABLE integers(i INTEGER)"));
	// create a database with 5 values
	REQUIRE(db.Execute("INSERT INTO integers VALUES (1), (2), (3), (4), (5)"));
	// 5 + 5 * 5 = 30 values
	REQUIRE(db.Execute("INSERT INTO integers SELECT i1.i FROM integers i1, integers i2"));
	// 30 + 30 * 30 = 930 values
	REQUIRE(db.Execute("INSERT INTO integers SELECT i1.i FROM integers i1, integers i2"));
	// run a thread that will run a big cross product
	thread t1(sqlite3_interrupt_fast, &db, &success);
	// wait a second and interrupt the db
	std::this_thread::sleep_for(std::chrono::milliseconds(1000));
	sqlite3_interrupt(db.db);
	// join the thread again
	t1.join();
	// the execution should have been cancelled
	REQUIRE(!success);
}
