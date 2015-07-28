#include "stdafx.h"

#include "data_provider.h"

#include "data_consumer.h"
#include "file_enum.h"
#include "file_reader_proxy.h"

DataProvider::DataProvider(unsigned max_open_files,
                           unsigned max_parallel_reads,
                           FileEnum* file_enum,
                           TaskQueue* task_queue,
                           TaskQueue* io_task_queue,
                           DataConsumer* consumer)
    : max_open_files_(max_open_files),
      max_parallel_reads_(max_parallel_reads),
      file_enum_(file_enum),
      task_queue_(task_queue),
      io_task_queue_(io_task_queue),
      consumer_(consumer),
      next_reader_(readers_.end()),
      next_file_id_(0),
      is_suspended_(false) {
}

DataProvider::~DataProvider() {
  assert(readers_.empty());
  assert(next_reader_ == readers_.end());
}

void DataProvider::Start() {
  for (unsigned i = 0; i < max_parallel_reads_; ++i) {
    io_buffers_.emplace_back(new IOBuffer());
    IOBuffer* io_buffer = io_buffers_.back().get();
    io_buffer->buffer.resize(kBlockSize);
    io_buffer->is_busy = false;
  }
  OpenNextFile();
  next_reader_ = readers_.begin();
}

void DataProvider::Suspend() {
  is_suspended_ = true;
}

void DataProvider::Resume() {
  is_suspended_ = false;
  ReadNextBlock();
}

void DataProvider::OnOpenFile(Readers::iterator reader_iter, bool result) {
  if (!result) {
    DeleteReader(reader_iter);
    return;
  }
  Reader* reader = reader_iter->get();
  reader->state = ReaderState::kIdle;
  consumer_->OnDataConsumerFileOpened(reader->file_id, reader->file_path);
  ReadNextBlock();
}

void DataProvider::OnReadFile(Readers::iterator reader_iter,
                              IOBuffer* io_buffer,
                              FileReaderResult read_result) {
  Reader* reader = reader_iter->get();
  io_buffer->is_busy = false;
  if (read_result.status != FileReaderStatus::kSuccess) {
    reader->state = ReaderState::kClosing;
    reader->file_reader->Close(
        std::bind(&DataProvider::OnCloseFile, this, reader_iter));
  } else {
    reader->state = ReaderState::kIdle;
    reader->read_offset += read_result.size;
    consumer_->OnDataConsumerBlockRead(reader->file_id, reader->file_path,
                                       io_buffer->buffer.data(),
                                       read_result.size);
  }
  ReadNextBlock();
}

void DataProvider::OnCloseFile(Readers::iterator reader_iter) {
  Reader* reader = reader_iter->get();
  consumer_->OnDataConsumerFileClosed(reader->file_id, reader->file_path);
  DeleteReader(reader_iter);
}

bool DataProvider::FindNextIdleReader() {
  if (readers_.empty()) {
    assert(false);
    return false;
  }
  Readers::iterator original_iter = next_reader_;
  while ((*next_reader_)->state != ReaderState::kIdle) {
    MoveNextReader();
    if (next_reader_ == original_iter)
      return false;
  }
  return true;
}

void DataProvider::MoveNextReader() {
  if (next_reader_ == readers_.end()) {
    assert(false);
    return;
  }
  ++next_reader_;
  if (next_reader_ == readers_.end())
    next_reader_ = readers_.begin();
}

void DataProvider::OpenNextFile() {
  while (readers_.size() < max_open_files_ && file_enum_->Next()) {
    if (file_enum_->IsDir())
      continue;
    Readers::iterator reader_iter =
        readers_.emplace(next_reader_, new Reader());
    Reader* reader = reader_iter->get();
    reader->file_reader.reset(new FileReaderProxy(task_queue_, io_task_queue_));
    reader->state = ReaderState::kOpening;
    reader->read_offset = 0;
    reader->file_path = file_enum_->current_path();
    reader->file_id = next_file_id_++;
    reader->file_reader->Open(file_enum_->current_path(),
                              std::bind(&DataProvider::OnOpenFile, this,
                                        reader_iter, std::placeholders::_1));
  }
  if (readers_.empty())
    consumer_->OnDataConsumerFinished();
}

void DataProvider::ReadNextBlock() {
  if (is_suspended_)
    return;
  DataProvider::IOBuffer* io_buffer;
  while ((io_buffer = FindFreeIOBuffer()) && FindNextIdleReader()) {
    Reader* reader = next_reader_->get();
    reader->state = ReaderState::kReading;
    io_buffer->is_busy = true;
    reader->file_reader->Read(
        reader->read_offset, kBlockSize, io_buffer->buffer.data(),
        std::bind(&DataProvider::OnReadFile, this, next_reader_, io_buffer,
                  std::placeholders::_1));
    MoveNextReader();
  }
}

void DataProvider::DeleteReader(Readers::iterator reader_iter) {
  if (next_reader_ == reader_iter)
    MoveNextReader();
  if (next_reader_ == reader_iter)
    next_reader_ = readers_.end();
  readers_.erase(reader_iter);
  OpenNextFile();
}

DataProvider::IOBuffer* DataProvider::FindFreeIOBuffer() {
  for (const std::unique_ptr<IOBuffer>& io_buffer : io_buffers_) {
    if (!io_buffer->is_busy)
      return io_buffer.get();
  }
  return nullptr;
}
