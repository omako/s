#pragma once

#include "file_reader.h"

class TaskQueue;

class FileReaderProxy {
 public:
  FileReaderProxy(TaskQueue* client_task_queue, TaskQueue* io_task_queue);
  ~FileReaderProxy();
  void Open(const std::wstring& file_path, std::function<void(bool)> callback);
  void Read(uint64_t offset,
            uint32_t size,
            uint8_t* buffer,
            FileReader::ReadCallback callback);
  void Close(std::function<void()> callback);

 private:
  FileReaderProxy(const FileReaderProxy&) = delete;
  FileReaderProxy& operator=(const FileReaderProxy&) = delete;

  void Open2(const std::wstring& file_path, std::function<void(bool)> callback);
  void OnReadCompleted(FileReader::ReadCallback callback,
                       FileReaderResult read_result);
  void Close2(std::function<void()> callback);

  TaskQueue* client_task_queue_;
  TaskQueue* io_task_queue_;
  FileReader* file_reader_;
};
