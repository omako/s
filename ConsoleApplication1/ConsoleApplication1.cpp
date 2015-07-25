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

int _tmain(int argc, _TCHAR* argv[]) {
  /*FileEnum file_enum(L"c:\\temp");
  while (file_enum.Next()) {
    std::wcout << file_enum.current_path() << L'\n';
  }*/
  extern void TestScanner();
  TestScanner();
  return 0;
}

class Scanner {
 public:
  Scanner(unsigned max_open_files,
          unsigned max_parallel_reads,
          FileEnum* file_enum,
          TaskQueue* task_queue,
          TaskQueue* io_task_queue,
          std::function<void()> finish_callback);
  ~Scanner();

  void Start();

 private:
  const uint32_t kBlockSize = 1 << 20;
  enum class ReaderState { kOpening, kReading, kClosing, kIdle };
  struct Reader {
    std::unique_ptr<FileReaderProxy> file_reader;
    ReaderState state;
    uint64_t read_offset;
    std::wstring file_path;  // For logging.
  };
  using Readers = std::list<std::unique_ptr<Reader>>;

  Scanner(const Scanner&) = delete;
  Scanner& operator=(const Scanner&) = delete;

  void OnOpenFile(Readers::iterator reader_iter, bool result);
  void OnReadFile(Readers::iterator reader_iter,
                  FileReader::ReadResult read_result);
  void OnCloseFile(Readers::iterator reader_iter);
  bool FindNextIdleReader();
  void MoveNextReader();
  void OpenNextFile();
  void ReadNextBlock();
  void DeleteReader(Readers::iterator reader_iter);

  const unsigned max_open_files_;
  const unsigned max_parallel_reads_;
  FileEnum* file_enum_;
  TaskQueue* task_queue_;
  TaskQueue* io_task_queue_;
  std::function<void()> finish_callback_;
  Readers readers_;
  Readers::iterator next_reader_;
  unsigned active_read_count_;
};

Scanner::Scanner(unsigned max_open_files,
                 unsigned max_parallel_reads,
                 FileEnum* file_enum,
                 TaskQueue* task_queue,
                 TaskQueue* io_task_queue,
                 std::function<void()> finish_callback)
    : max_open_files_(max_open_files),
      max_parallel_reads_(max_parallel_reads),
      file_enum_(file_enum),
      task_queue_(task_queue),
      io_task_queue_(io_task_queue),
      finish_callback_(finish_callback),
      next_reader_(readers_.end()),
      active_read_count_(0) {
}

Scanner::~Scanner() {
  assert(readers_.empty());
  assert(next_reader_ == readers_.end());
  assert(active_read_count_ == 0);
}

void Scanner::Start() {
  OpenNextFile();
  next_reader_ = readers_.begin();
}

void Scanner::OnOpenFile(Readers::iterator reader_iter, bool result) {
  //std::wcout << "Open: result=" << result << ", " << (*reader_iter)->file_path
  //           << std::endl;
  if (!result) {
    DeleteReader(reader_iter);
    return;
  }
  Reader* reader = reader_iter->get();
  reader->state = ReaderState::kIdle;
  ReadNextBlock();
}

void Scanner::OnReadFile(Readers::iterator reader_iter,
                         FileReader::ReadResult read_result) {
  //std::wcout << "Read: status=" << read_result.status
  //           << ", size=" << read_result.size << ", "
  //           << (*reader_iter)->file_path << std::endl;
  --active_read_count_;
  Reader* reader = reader_iter->get();
  if (read_result.status != FileReader::FILE_READ_SUCCESS) {
    reader->state = ReaderState::kClosing;
    reader->file_reader->Close(
        std::bind(&Scanner::OnCloseFile, this, reader_iter));
  } else {
    reader->state = ReaderState::kIdle;
    reader->read_offset += read_result.size;
  }
  ReadNextBlock();
}

void Scanner::OnCloseFile(Readers::iterator reader_iter) {
  //std::wcout << "Close: " << (*reader_iter)->file_path << std::endl;
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
    reader->file_reader->Open(file_enum_->current_path(),
                              std::bind(&Scanner::OnOpenFile, this, reader_iter,
                                        std::placeholders::_1));
  }
  if (readers_.empty())
    finish_callback_();
}

void Scanner::ReadNextBlock() {
  while (active_read_count_ < max_parallel_reads_ && FindNextIdleReader()) {
    Reader* reader = next_reader_->get();
    reader->state = ReaderState::kReading;
    ++active_read_count_;
    reader->file_reader->Read(reader->read_offset, kBlockSize,
                              std::bind(&Scanner::OnReadFile, this,
                                        next_reader_, std::placeholders::_1));
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

void IOThread(TaskQueue* io_task_queue) {
  io_task_queue->Run();
}

void FinishCallback(TaskQueue* task_queue) {
  task_queue->PostQuitTask();
}

void TestScanner() {
  std::unique_ptr<TaskQueue> task_queue(new TaskQueueImpl());
  std::unique_ptr<TaskQueue> io_task_queue(new IOTaskQueue());
  std::thread io_thread(std::bind(&IOThread, io_task_queue.get()));
  FileEnum file_enum(L"C:\\GOG Games\\The Witcher 3 Wild Hunt");
  Scanner scanner(16, 8, &file_enum, task_queue.get(), io_task_queue.get(),
                  std::bind(&FinishCallback, task_queue.get()));
  scanner.Start();
  task_queue->Run();
  io_task_queue->PostQuitTask();
  io_thread.join();
}
