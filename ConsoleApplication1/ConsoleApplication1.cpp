// ConsoleApplication1.cpp : Defines the entry point for the console
// application.
//

#include "stdafx.h"

#include "file_enum.h"
#include "file_reader_proxy.h"
#include "task.h"

struct Worker {
  std::thread thread;
  std::unique_ptr<TaskQueue> task_queue;
  Worker() {}
  Worker(Worker&& worker)
      : thread(std::move(worker.thread)),
        task_queue(std::move(worker.task_queue)) {}
};

int test_task() {
  static int counter = 0;
  ++counter;
  return counter;
}

void test_reply(const int& reply_value,
                size_t num_threads,
                TaskQueue* reply_queue) {
  if (reply_value == num_threads)
    reply_queue->PostQuitTask();
}

void test_producer(const std::vector<Worker>& workers, TaskQueue* reply_queue) {
  for (unsigned i = 0; i < workers.size(); ++i) {
    workers[i].task_queue->Post(std::unique_ptr<Task>(new TaskWithReply<int>(
        &test_task, std::bind(&test_reply, std::placeholders::_1,
                              workers.size(), reply_queue),
        reply_queue)));
    workers[i].task_queue->PostQuitTask();
  }
}

void test() {
  const unsigned num_threads = std::thread::hardware_concurrency();
  std::vector<Worker> workers;
  workers.reserve(num_threads);
  for (unsigned i = 0; i < num_threads; ++i) {
    Worker worker;
    worker.task_queue.reset(new TaskQueueImpl());
    worker.thread = std::thread(&TaskQueue::Run, worker.task_queue.get());
    workers.push_back(std::move(worker));
  }
  std::unique_ptr<TaskQueue> reply_queue(new TaskQueueImpl());
  reply_queue->Post(std::unique_ptr<Task>(new SimpleTask(
      std::bind(&test_producer, std::ref(workers), reply_queue.get()))));
  reply_queue->Run();
  for (unsigned i = 0; i < num_threads; ++i) {
    workers[i].thread.join();
  }
}

int wmain(int argc, wchar_t* argv[]) {
  extern void TestScanner(const std::wstring& path);
  TestScanner(argv[1]);
  return 0;
}

using FileId = uint64_t;
const FileId kInvalidFileId = std::numeric_limits<FileId>::max();

class DataConsumer {
 public:
  virtual ~DataConsumer() {}
  virtual void OnFileOpened(FileId file_id, const std::wstring& file_path) = 0;
  virtual void OnBlockRead(FileId file_id,
                           const std::wstring& file_path,
                           uint8_t* data,
                           uint32_t size) = 0;
  virtual void OnFileClosed(FileId file_id, const std::wstring& file_path) = 0;
  virtual void OnDataEnd() = 0;
};

class Scanner {
 public:
  Scanner(unsigned max_open_files,
          unsigned max_parallel_reads,
          FileEnum* file_enum,
          TaskQueue* task_queue,
          TaskQueue* io_task_queue,
          DataConsumer* consumer);
  ~Scanner();

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
  using IOBuffers = std::vector<IOBuffer>;

  Scanner(const Scanner&) = delete;
  Scanner& operator=(const Scanner&) = delete;

  void OnOpenFile(Readers::iterator reader_iter, bool result);
  void OnReadFile(Readers::iterator reader_iter,
                  IOBuffer* io_buffer,
                  FileReader::ReadResult read_result);
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

Scanner::Scanner(unsigned max_open_files,
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
      next_file_id_(0) {
}

Scanner::~Scanner() {
  assert(readers_.empty());
  assert(next_reader_ == readers_.end());
}

void Scanner::Start() {
  for (unsigned i = 0; i < max_parallel_reads_; ++i) {
    io_buffers_.emplace_back();
    IOBuffer& io_buffer = io_buffers_.back();
    io_buffer.buffer.resize(kBlockSize);
    io_buffer.is_busy = false;
  }
  OpenNextFile();
  next_reader_ = readers_.begin();
}

void Scanner::OnOpenFile(Readers::iterator reader_iter, bool result) {
  if (!result) {
    DeleteReader(reader_iter);
    return;
  }
  Reader* reader = reader_iter->get();
  reader->state = ReaderState::kIdle;
  consumer_->OnFileOpened(reader->file_id, reader->file_path);
  ReadNextBlock();
}

void Scanner::OnReadFile(Readers::iterator reader_iter,
                         IOBuffer* io_buffer,
                         FileReader::ReadResult read_result) {
  Reader* reader = reader_iter->get();
  io_buffer->is_busy = false;
  if (read_result.status != FileReader::FILE_READ_SUCCESS) {
    reader->state = ReaderState::kClosing;
    reader->file_reader->Close(
        std::bind(&Scanner::OnCloseFile, this, reader_iter));
  } else {
    reader->state = ReaderState::kIdle;
    reader->read_offset += read_result.size;
    consumer_->OnBlockRead(reader->file_id, reader->file_path,
                           io_buffer->buffer.data(), read_result.size);
  }
  ReadNextBlock();
}

void Scanner::OnCloseFile(Readers::iterator reader_iter) {
  consumer_->OnFileOpened((*reader_iter)->file_id, (*reader_iter)->file_path);
  DeleteReader(reader_iter);
}

bool Scanner::FindNextIdleReader() {
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

void Scanner::MoveNextReader() {
  if (next_reader_ == readers_.end()) {
    assert(false);
    return;
  }
  ++next_reader_;
  if (next_reader_ == readers_.end())
    next_reader_ = readers_.begin();
}

void Scanner::OpenNextFile() {
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
                              std::bind(&Scanner::OnOpenFile, this, reader_iter,
                                        std::placeholders::_1));
  }
  if (readers_.empty())
    consumer_->OnDataEnd();
}

void Scanner::ReadNextBlock() {
  Scanner::IOBuffer* io_buffer;
  while ((io_buffer = FindFreeIOBuffer()) && FindNextIdleReader()) {
    Reader* reader = next_reader_->get();
    reader->state = ReaderState::kReading;
    io_buffer->is_busy = true;
    reader->file_reader->Read(
        reader->read_offset, kBlockSize, io_buffer->buffer.data(),
        std::bind(&Scanner::OnReadFile, this, next_reader_, io_buffer,
                  std::placeholders::_1));
    MoveNextReader();
  }
}

void Scanner::DeleteReader(Readers::iterator reader_iter) {
  if (next_reader_ == reader_iter)
    MoveNextReader();
  if (next_reader_ == reader_iter)
    next_reader_ = readers_.end();
  readers_.erase(reader_iter);
  OpenNextFile();
}

Scanner::IOBuffer* Scanner::FindFreeIOBuffer() {
  for (Scanner::IOBuffer& io_buffer : io_buffers_) {
    if (!io_buffer.is_busy)
      return &io_buffer;
  }
  return nullptr;
}











class DataConsumerImpl : public DataConsumer {
 public:
  DataConsumerImpl(unsigned max_open_files,
                   unsigned max_parallel_reads,
                   unsigned num_processors,
                   TaskQueue* task_queue,
                   TaskQueue* io_task_queue,
                   const std::wstring& path);

  void Start();

 private:
  struct Processor {
    std::thread thread;
    std::unique_ptr<TaskQueue> task_queue;
    bool is_busy;
    FileId file_id;
  };
  using ProcessingResult = int;
  using ProcessingContext = int;
  using DataBlock = std::vector<uint8_t>;
  struct File {
    std::wstring path;
    bool is_closed;
    std::list<std::shared_ptr<DataBlock>> data_blocks;
    std::unique_ptr<ProcessingContext> context;
  };
  using Files = std::unordered_map<FileId, std::unique_ptr<File>>;
  using ProcessingQueue = std::deque<FileId>;

  // DataConsumer
  void OnFileOpened(FileId file_id, const std::wstring& file_path) override;
  void OnBlockRead(FileId file_id,
                   const std::wstring& file_path,
                   uint8_t* data,
                   uint32_t size) override;
  void OnFileClosed(FileId file_id, const std::wstring& file_path) override;
  void OnDataEnd() override;

  void ProcessingThread();
  ProcessingResult Process(std::shared_ptr<DataBlock> data_block,
                           ProcessingContext* context);
  void OnProcessingReply(Processor* processor, const ProcessingResult& result);
  FileId ProcessNextFile();
  void GiveTasksToProcessors();

  const unsigned num_processors_;
  TaskQueue* task_queue_;
  TaskQueue* io_task_queue_;
  FileEnum file_enum_;
  Scanner scanner_;
  Files files_;
  std::unordered_set<FileId> files_in_processing_;
  ProcessingQueue processing_queue_;
  size_t total_size_;
  std::vector<std::unique_ptr<Processor>> processors_;
};

DataConsumerImpl::DataConsumerImpl(unsigned max_open_files,
                                   unsigned max_parallel_reads,
                                   unsigned num_processors,
                                   TaskQueue* task_queue,
                                   TaskQueue* io_task_queue,
                                   const std::wstring& path)
    : num_processors_(num_processors),
      task_queue_(task_queue),
      file_enum_(path),
      scanner_(max_open_files,
               max_parallel_reads,
               &file_enum_,
               task_queue,
               io_task_queue,
               this),
      total_size_(0) {
}

void DataConsumerImpl::Start() {
  for (unsigned i = 0; i < num_processors_; ++i) {
    processors_.emplace_back(new Processor());
    Processor* processor = processors_.back().get();
    processor->is_busy = false;
    processor->task_queue.reset(new TaskQueueImpl());
    processor->file_id = kInvalidFileId;
    processor->thread =
        std::thread(std::bind(&DataConsumerImpl::ProcessingThread, this));
  }
  scanner_.Start();
}

void DataConsumerImpl::OnFileOpened(FileId file_id,
                                    const std::wstring& file_path) {
  File* file = files_[file_id].get();
  file->path = file_path;
  file->is_closed = false;
  processing_queue_.push_back(file_id);
}

void DataConsumerImpl::OnBlockRead(FileId file_id,
                                   const std::wstring& file_path,
                                   uint8_t* data,
                                   uint32_t size) {
  files_[file_id]->data_blocks.emplace_back(new DataBlock(data, data + size));
  total_size_ += size;
  GiveTasksToProcessors();
}

void DataConsumerImpl::OnFileClosed(FileId file_id,
                                    const std::wstring& file_path) {
  files_[file_id]->is_closed = true;
  GiveTasksToProcessors();
}

void DataConsumerImpl::OnDataEnd() {
  task_queue_->PostQuitTask();
}

void DataConsumerImpl::ProcessingThread() {
  task_queue_->Run();
}

DataConsumerImpl::ProcessingResult DataConsumerImpl::Process(
    std::shared_ptr<DataBlock> data_block,
    ProcessingContext* context) {
  return DataConsumerImpl::ProcessingResult();
}

void DataConsumerImpl::OnProcessingReply(Processor* processor,
                                         const ProcessingResult& result) {
  processor->is_busy = false;
  GiveTasksToProcessors();
}

FileId DataConsumerImpl::ProcessNextFile() {
  for (ProcessingQueue::iterator file_iter = processing_queue_.begin();
       file_iter != processing_queue_.end();) {
    FileId file_id = *file_iter;
    File* file = files_[file_id].get();
    if (files_in_processing_.find(file_id) != files_in_processing_.end()) {
      ++file_iter;
      continue;
    }
    if (file->data_blocks.empty()) {
      if (file->is_closed) {
        files_.erase(file_id);
        file_iter = processing_queue_.erase(file_iter);
      } else {
        ++file_iter;
      }
      continue;
    }
    processing_queue_.erase(file_iter);
    files_in_processing_.insert(file_id);
    if (!file->context)
      file->context.reset(new ProcessingContext());
    return file_id;
  }
  return kInvalidFileId;
}

void DataConsumerImpl::GiveTasksToProcessors() {
  for (const std::unique_ptr<Processor>& processor : processors_) {
    if (processor->is_busy)
      continue;
    FileId file_id;
    if (processor->file_id == kInvalidFileId) {
      file_id = ProcessNextFile();
    } else {
      File* processor_file = files_[processor->file_id].get();
      if (processor_file->data_blocks.empty()) {
        files_in_processing_.erase(processor->file_id);
        if (processor_file->is_closed)
          files_.erase(processor->file_id);
        else
          processing_queue_.push_front(file_id);
        file_id = ProcessNextFile();
      } else {
        file_id = processor->file_id;
      }
    }
    if (file_id == kInvalidFileId)
      break;
    File* file = files_[file_id].get();
    std::shared_ptr<DataBlock> data_block(file->data_blocks.front());
    file->data_blocks.pop_front();
    processor->is_busy = true;
    processor->task_queue->Post(
        std::unique_ptr<Task>(new TaskWithReply<ProcessingResult>(
            std::bind(&DataConsumerImpl::Process, this, data_block,
                      file->context.get()),
            std::bind(&DataConsumerImpl::OnProcessingReply, this,
                      processor.get(), std::placeholders::_1),
            task_queue_)));
  }
}










void IOThread(TaskQueue* io_task_queue) {
  io_task_queue->Run();
}

void TestScanner(const std::wstring& path) {
  std::unique_ptr<TaskQueue> task_queue(new TaskQueueImpl());
  std::unique_ptr<TaskQueue> io_task_queue(new IOTaskQueue());
  std::thread io_thread(std::bind(&IOThread, io_task_queue.get()));
  DataConsumerImpl consumer(16, 8, 8, task_queue.get(), io_task_queue.get(),
                            path);
  consumer.Start();
  task_queue->Run();
  io_task_queue->PostQuitTask();
  io_thread.join();
}
