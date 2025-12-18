#include "fix_file_reader.hpp"
#include "duckdb/common/exception.hpp"
#include <mutex>

namespace duckdb {

FixFileReader::FixFileReader() : line_number_(0), buffer_offset_(0), file_done_(false) {
}

bool FixFileReader::OpenNextFile(FileSystem &fs, const vector<string> &files, idx_t &file_index, std::mutex &lock) {
	// Close any existing file
	Close();

	// Get next file with lock
	std::lock_guard<std::mutex> guard(lock);

	if (file_index >= files.size()) {
		return false; // No more files
	}

	current_file_ = files[file_index];
	file_index++;

	// Open file using DuckDB FileSystem API (supports S3, HTTP, etc.)
	file_handle_ = fs.OpenFile(current_file_, FileFlags::FILE_FLAGS_READ);
	line_number_ = 0;
	file_done_ = false;
	buffer_.clear();
	buffer_offset_ = 0;

	return true;
}

bool FixFileReader::ReadLine(string &line) {
	if (!file_handle_) {
		return false; // No file open
	}

	line.clear();
	bool found_line = false;

	while (!file_done_) {
		// Check if we need to read more data into buffer
		if (buffer_offset_ >= buffer_.size()) {
			// Read next chunk
			buffer_.resize(BUFFER_SIZE);
			idx_t bytes_read = file_handle_->Read((void *)buffer_.data(), BUFFER_SIZE);

			if (bytes_read == 0) {
				// End of file
				file_done_ = true;
				if (!line.empty()) {
					found_line = true;
					line_number_++;
				}
				break;
			}

			buffer_.resize(bytes_read);
			buffer_offset_ = 0;
		}

		// Find newline in buffer
		size_t newline_pos = buffer_.find('\n', buffer_offset_);
		if (newline_pos != string::npos) {
			// Found newline
			line.append(buffer_, buffer_offset_, newline_pos - buffer_offset_);
			buffer_offset_ = newline_pos + 1;
			found_line = true;
			line_number_++;
			break;
		} else {
			// No newline in current buffer, append all remaining data
			line.append(buffer_, buffer_offset_, buffer_.size() - buffer_offset_);
			buffer_offset_ = buffer_.size();
		}
	}

	if (!found_line) {
		return false; // End of file, no more lines
	}

	// Remove trailing carriage return if present (for Windows line endings)
	if (!line.empty() && line.back() == '\r') {
		line.pop_back();
	}

	return true;
}

void FixFileReader::Close() {
	file_handle_.reset();
	current_file_.clear();
	line_number_ = 0;
	buffer_.clear();
	buffer_offset_ = 0;
	file_done_ = false;
}

} // namespace duckdb
