#include "stdafx.h"

#include "file_reader_proxy.h"

#include "task.h"

FileReaderProxy::FileReaderProxy(TaskQueue* client_task_queue,
                                 TaskQueue* io_task_queue)
    : client_task_queue_(client_task_queue),
      io_task_queue_(io_task_queue),
      file_reader_(nullptr) {
}

FileReaderProxy::~FileReaderProxy() {
  assert(file_reader_ == nullptr);
}

void FileReaderProxy::Open(const std::wstring& file_path,
                           std::function<void(bool)> callback) {
  io_task_queue_->Post(std::unique_ptr<Task>(new SimpleTask(
      std::bind(&FileReaderProxy::Open2, this, file_path, callback))));
}

void FileReaderProxy::Read(uint64_t offset,
                           uint32_t size,
                           uint8_t* buffer,
                           FileReader::ReadCallback callback) {
  assert(file_reader_ != nullptr);
  FileReader::ReadCallback cb = std::bind(
      &FileReaderProxy::OnReadCompleted, this, callback, std::placeholders::_1);
  io_task_queue_->Post(std::unique_ptr<Task>(new SimpleTask(
      std::bind(&FileReader::Read, file_reader_, offset, size, buffer, cb))));
}

void FileReaderProxy::Close(std::function<void()> callback) {
  io_task_queue_->Post(std::unique_ptr<Task>(new SimpleTask(
      std::bind(&FileReaderProxy::Close2, this, callback))));
}

void FileReaderProxy::Open2(const std::wstring& file_path,
                            std::function<void(bool)> callback) {
  assert(file_reader_ == nullptr);
  file_reader_ = new FileReader();
  bool res = file_reader_->Open(file_path);
  if (!res) {
    delete file_reader_;
    file_reader_ = nullptr;
  }
  client_task_queue_->Post(
      std::unique_ptr<Task>(new SimpleTask(std::bind(callback, res))));
}

void FileReaderProxy::OnReadCompleted(FileReader::ReadCallback callback,
                                      FileReader::ReadResult read_result) {
  client_task_queue_->Post(std::unique_ptr<Task>(
      new SimpleTask(std::bind(callback, read_result))));
}

void FileReaderProxy::Close2(std::function<void()> callback) {
  file_reader_->Close();
  delete file_reader_;
  file_reader_ = nullptr;
  client_task_queue_->Post(std::unique_ptr<Task>(new SimpleTask(callback)));
}
