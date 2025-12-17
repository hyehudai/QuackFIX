#pragma once

#include "duckdb/common/file_system.hpp"
#include <string>
#include <mutex>

namespace duckdb {

// Helper class for reading FIX log files line by line
// Handles buffering and various line ending formats (\n, \r\n, \r)
class FixFileReader {
public:
	FixFileReader();

	// Open a file for reading
	// Returns true on success, false if no more files available
	bool OpenNextFile(FileSystem &fs, const vector<string> &files, idx_t &file_index, std::mutex &lock);

	// Read the next line from the current file
	// Returns true if a line was read, false if end of file reached
	// Line endings (\n, \r\n, \r) are automatically stripped
	bool ReadLine(string &line);

	// Get current file path
	const string &GetCurrentFile() const {
		return current_file_;
	}

	// Get current line number (1-indexed)
	idx_t GetLineNumber() const {
		return line_number_;
	}

	// Close current file
	void Close();

	// Check if file is open
	bool IsOpen() const {
		return file_handle_ != nullptr;
	}

private:
	// File handle
	unique_ptr<FileHandle> file_handle_;

	// Current file path
	string current_file_;

	// Line number counter (1-indexed)
	idx_t line_number_;

	// Read buffer
	string buffer_;
	idx_t buffer_offset_;
	bool file_done_;

	// Buffer size for file reads
	static constexpr idx_t BUFFER_SIZE = 8192;
};

} // namespace duckdb
