#pragma once

enum class FileReaderStatus {
  kSuccess,
  kEOF,
  kError
};

struct FileReaderResult {
  FileReaderStatus status;
  uint32_t size;
};
