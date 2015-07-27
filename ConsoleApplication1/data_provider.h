#pragma once

#include "file_id.h"
#include "file_reader_result.h"

class DataConsumer;
class FileEnum;
class FileReaderProxy;
class TaskQueue;

class DataProvider {
 public:
  DataProvider(unsigned max_open_files,
               unsigned max_parallel_reads,
               FileEnum* file_enum,
               TaskQueue* task_queue,
               TaskQueue* io_task_queue,
               DataConsumer* consumer);
  ~DataProvider();

  void Start();

 private:
  const uint32_t kBlockSize = 1 << 20;
  enum class ReaderState { kOpening, kReading, kClosing, kIdle };
  struct Reader {
    std::unique_ptr<FileReaderProxy> file_reader;
    ReaderState state;
    uint64_t read_offset;
    std::wstring file_path;
    FileId file_id;
  };
  using Readers = std::list<std::unique_ptr<Reader>>;
  struct IOBuffer {
    std::vector<uint8_t> buffer;
    bool is_busy;
  };
  using IOBuffers = std::vector<std::unique_ptr<IOBuffer>>;

  DataProvider(const DataProvider&) = delete;
  DataProvider& operator=(const DataProvider&) = delete;

  void OnOpenFile(Readers::iterator reader_iter, bool result);
  void OnReadFile(Readers::iterator reader_iter,
                  IOBuffer* io_buffer,
                  FileReaderResult read_result);
  void OnCloseFile(Readers::iterator reader_iter);
  bool FindNextIdleReader();
  void MoveNextReader();
  void OpenNextFile();
  void ReadNextBlock();
  void DeleteReader(Readers::iterator reader_iter);
  IOBuffer* FindFreeIOBuffer();

  const unsigned max_open_files_;
  const unsigned max_parallel_reads_;
  FileEnum* file_enum_;
  TaskQueue* task_queue_;
  TaskQueue* io_task_queue_;
  DataConsumer* consumer_;
  Readers readers_;
  IOBuffers io_buffers_;
  Readers::iterator next_reader_;
  FileId next_file_id_;
};
