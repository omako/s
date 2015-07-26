#pragma once

class IOBuffer;

class FileReader {
 public:
  enum Status {
    FILE_READ_SUCCESS,
    FILE_READ_EOF,
    FILE_READ_ERROR
  };
  struct ReadResult {
    Status status;
    uint32_t size;
  };
  using ReadCallback = std::function<void(ReadResult)>;

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
  void OnDataRead(DWORD error_code,
                  uint32_t size_read,
                  IORequest* io_request);
  void DeleteRequest(IORequest* io_request);

  HANDLE file_handle_;
  IORequests io_requests_;
};
