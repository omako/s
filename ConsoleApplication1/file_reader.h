#pragma once

#include "file_reader_result.h"

class FileReader {
 public:
  using ReadCallback = std::function<void(FileReaderResult)>;

  FileReader();
  ~FileReader();
  bool Open(const std::wstring& file_path);
  void Read(uint64_t offset,
            uint32_t size,
            uint8_t* buffer,
            ReadCallback callback);
  void Close();

 private:
  struct IORequest {
    OVERLAPPED overlapped;
    FileReader* object;
    ReadCallback callback;
  };
  using IORequests = std::vector<IORequest*>;

  FileReader(const FileReader&) = delete;
  FileReader& operator=(const FileReader&) = delete;

  static VOID CALLBACK FileIOCompletionRoutine(DWORD error_code,
                                               DWORD size_read,
                                               LPOVERLAPPED overlapped);
  void OnDataRead(DWORD error_code, uint32_t size_read, IORequest* io_request);
  void DeleteRequest(IORequest* io_request);

  HANDLE file_handle_;
  IORequests io_requests_;
};
