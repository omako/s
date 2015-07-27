#include "stdafx.h"

#include "file_reader.h"

FileReader::FileReader() : file_handle_(INVALID_HANDLE_VALUE) {
}

FileReader::~FileReader() {
  Close();
}

bool FileReader::Open(const std::wstring& file_path) {
  Close();
  file_handle_ = CreateFile(
      file_path.c_str(), FILE_READ_DATA,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
      OPEN_EXISTING, FILE_FLAG_OVERLAPPED | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
  return file_handle_ != INVALID_HANDLE_VALUE;
}

void FileReader::Read(uint64_t offset,
                      uint32_t size,
                      uint8_t* buffer,
                      ReadCallback callback) {
  IORequest* io_request = new IORequest();
  io_requests_.push_back(io_request);
  memset(&io_request->overlapped, 0, sizeof(io_request->overlapped));
  io_request->overlapped.Offset = offset & 0xFFFFFFFF;
  io_request->overlapped.OffsetHigh = (offset >> 32) & 0xFFFFFFFF;
  io_request->object = this;
  io_request->callback = callback;
  BOOL res = ReadFileEx(file_handle_, buffer, size, &io_request->overlapped,
                        &FileIOCompletionRoutine);
  if (!res) {
    DeleteRequest(io_request);
    FileReaderResult result = {FileReaderStatus::kError};
    callback(result);
  }
}

void FileReader::Close() {
  for (IORequest* io_request : io_requests_)
    delete io_request;
  io_requests_.clear();
  if (file_handle_ != INVALID_HANDLE_VALUE) {
    BOOL res = CloseHandle(file_handle_);
    assert(res);
    file_handle_ = INVALID_HANDLE_VALUE;
  }
}

// static
VOID CALLBACK FileReader::FileIOCompletionRoutine(DWORD error_code,
                                                  DWORD size_read,
                                                  LPOVERLAPPED overlapped) {
  IORequest* io_request = reinterpret_cast<IORequest*>(overlapped);
  io_request->object->OnDataRead(error_code, size_read, io_request);
}

void FileReader::OnDataRead(DWORD error_code,
                            uint32_t size_read,
                            IORequest* io_request) {
  FileReaderResult result;
  if (error_code == ERROR_SUCCESS) {
    result.status = FileReaderStatus::kSuccess;
    result.size = size_read;
  } else if (error_code == ERROR_HANDLE_EOF) {
    result.status = FileReaderStatus::kEOF;
    result.size = 0;
  } else {
    result.status = FileReaderStatus::kError;
    result.size = 0;
  }
  io_request->callback(result);
  DeleteRequest(io_request);
}

void FileReader::DeleteRequest(IORequest* io_request) {
  IORequests::iterator io_requests_iter =
      std::find(io_requests_.begin(), io_requests_.end(), io_request);
  if (io_requests_iter == io_requests_.end()) {
    assert(false);
    return;
  }
  delete *io_requests_iter;
  io_requests_.erase(io_requests_iter);
}
