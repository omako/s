// ConsoleApplication1.cpp : Defines the entry point for the console
// application.
//

#include "stdafx.h"

#include "task.h"
#include "processing_context.h"
#include "processing_manager.h"

extern "C" {
#include "sregex/sregex.h"
}

struct Result {
  uint64_t offset;
  uint64_t size;
};

class Report {
 public:
  void ReportFound(const std::wstring& file_path, const Result& result);
};

void Report::ReportFound(const std::wstring& file_path, const Result& result) {
  std::wcout << file_path << " @ " << result.offset << ", len=" << result.size
             << std::endl;
}

class ProcessingContextImpl : public ProcessingContext {
 public:
  ProcessingContextImpl(const std::string& search_text,
                        Report* report,
                        const std::wstring& file_path);

 private:
  void ProcessDataBlockPhase1(std::shared_ptr<DataBlock> data_block) override;
  void MarkEndOfData() override;
  void ProcessDataBlockPhase2() override;

  std::string search_text_;
  Report* report_;
  std::wstring file_path_;
  std::vector<uint8_t> backup_buffer_;
  std::vector<uint64_t> found_;
  uint64_t current_offset_;
};

ProcessingContextImpl::ProcessingContextImpl(const std::string& search_text,
                                             Report* report,
                                             const std::wstring& file_path)
    : search_text_(search_text),
      report_(report),
      file_path_(file_path),
      current_offset_(0) {
}

void ProcessingContextImpl::ProcessDataBlockPhase1(
    std::shared_ptr<DataBlock> data_block) {
  size_t data_size = backup_buffer_.size() + data_block->size();
  if (data_size > search_text_.size() - 1)
    data_size -= search_text_.size() - 1;
  else
    data_size = 0;
  for (size_t data_index = 0; data_index < data_size; ++data_index) {
    bool is_equal;
    for (size_t text_index = 0; text_index < search_text_.size(); ++text_index) {
      size_t char_index = data_index + text_index;
      char ch;
      if (char_index < backup_buffer_.size()) {
        ch = backup_buffer_[char_index];
      } else {
        char_index -= backup_buffer_.size();
        ch = data_block->data()[char_index];
      }
      is_equal = (ch == search_text_[text_index]);
      if (!is_equal)
        break;
    }
    if (is_equal) {
      found_.push_back(current_offset_ + data_index - backup_buffer_.size());
      data_index += search_text_.size() - 1;
    }
  }
  current_offset_ += data_block->size();
  if (backup_buffer_.size() < search_text_.size() - 1)
    backup_buffer_.resize(search_text_.size() - 1);
  if (backup_buffer_.size() <= data_block->size())
    memcpy(backup_buffer_.data(),
           data_block->data() + data_block->size() - backup_buffer_.size(),
           backup_buffer_.size());
}

void ProcessingContextImpl::MarkEndOfData() {
}

void ProcessingContextImpl::ProcessDataBlockPhase2() {
  for (uint64_t file_offset : found_) {
    Result result;
    result.offset = file_offset;
    result.size = search_text_.size();
    report_->ReportFound(file_path_, result);
  }
  found_.clear();
}

class ProcessingContextFactoryImpl : public ProcessingContextFactory {
 public:
  ProcessingContextFactoryImpl(const std::string& search_text, Report* report);

 private:
  ProcessingContext* CreateProcessingContext(
      const std::wstring& file_path) override;

  std::string search_text_;
  Report* report_;
};

ProcessingContextFactoryImpl::ProcessingContextFactoryImpl(
    const std::string& search_text,
    Report* report)
    : search_text_(search_text), report_(report) {
}

ProcessingContext* ProcessingContextFactoryImpl::CreateProcessingContext(
    const std::wstring& file_path) {
  return new ProcessingContextImpl(search_text_, report_, file_path);
}


class RegexSearch : public ProcessingContext {
 public:
  RegexSearch(const std::string& regex,
              Report* report,
              const std::wstring& file_path);
  ~RegexSearch();

 private:
  void ProcessDataBlockPhase1(std::shared_ptr<DataBlock> data_block) override;
  void MarkEndOfData() override;
  void ProcessDataBlockPhase2() override;

  std::string regex_;
  Report* report_;
  std::wstring file_path_;
  std::vector<Result> found_;
  uint64_t block_offset_;
  uint64_t match_offset_;
  sre_pool_t * sre_pool_;
  sre_vm_thompson_ctx_t* sre_ctx_;
};

RegexSearch::RegexSearch(const std::string& regex,
                         Report* report,
                         const std::wstring& file_path)
    : regex_(regex),
      report_(report),
      file_path_(file_path),
      block_offset_(0),
      match_offset_(0),
      sre_pool_(nullptr),
      sre_ctx_(nullptr) {
  sre_pool_ = sre_create_pool(4096);
  assert(sre_pool_);
  sre_uint_t ncaps;
  sre_int_t err_offset;
  sre_regex_t* sre_regex = sre_regex_parse(
      sre_pool_,
      const_cast<sre_char*>(reinterpret_cast<const sre_char*>(regex_.c_str())),
      &ncaps, 0, &err_offset);
  assert(sre_regex);
  sre_program_t* sre_program = sre_regex_compile(sre_pool_, sre_regex);
  assert(sre_program);
  sre_ctx_ = sre_vm_thompson_create_ctx(sre_pool_, sre_program);
  assert(sre_ctx_);
}

RegexSearch::~RegexSearch() {
  sre_destroy_pool(sre_pool_);
}

void RegexSearch::ProcessDataBlockPhase1(
    std::shared_ptr<DataBlock> data_block) {
  sre_char* ch_ptr = data_block->data();
  for (uint32_t offset = 0; offset < data_block->size(); ++offset, ++ch_ptr) {
    sre_int_t res = sre_vm_thompson_exec(sre_ctx_, ch_ptr, 1, 0);
    if (res == SRE_OK) {
      Result result;
      result.offset = match_offset_;
      result.size = block_offset_ + offset - match_offset_ + 1;
      found_.push_back(result);
    } else if (res == SRE_DECLINED) {
      match_offset_ = block_offset_ + offset + 1;
    } else if (res == SRE_AGAIN) {
    } else {
      assert(false);
    }
  }
  block_offset_ += data_block->size();
}

void RegexSearch::MarkEndOfData() {
  sre_int_t res = sre_vm_thompson_exec(sre_ctx_, nullptr, 0, 1);
  if (res == SRE_OK) {
    Result result;
    result.offset = match_offset_;
    result.size = block_offset_ - match_offset_ + 1;
    found_.push_back(result);
  } else if (res == SRE_DECLINED) {
  } else if (res == SRE_AGAIN) {
  } else {
    assert(false);
  }
}

void RegexSearch::ProcessDataBlockPhase2() {
  for (const Result& result : found_)
    report_->ReportFound(file_path_, result);
  found_.clear();
}

class RegexSearchFactoryImpl : public ProcessingContextFactory {
 public:
  RegexSearchFactoryImpl(const std::string& regex, Report* report);

 private:
  ProcessingContext* CreateProcessingContext(
      const std::wstring& file_path) override;

  std::string regex_;
  Report* report_;
};

RegexSearchFactoryImpl::RegexSearchFactoryImpl(
    const std::string& search_text,
    Report* report)
    : regex_(search_text), report_(report) {
}

ProcessingContext* RegexSearchFactoryImpl::CreateProcessingContext(
    const std::wstring& file_path) {
  return new RegexSearch(regex_, report_, file_path);
}


void Test(const std::wstring& path, const std::string& search_text) {
  std::unique_ptr<TaskQueue> task_queue(new TaskQueueImpl());
  std::unique_ptr<TaskQueue> io_task_queue(new IOTaskQueue());
  std::thread io_thread([&io_task_queue] { io_task_queue->Run(); });
  Report report;
  RegexSearchFactoryImpl factory(search_text, &report);
  ProcessingManager consumer(
      std::thread::hardware_concurrency(), std::thread::hardware_concurrency(),
      std::thread::hardware_concurrency(), task_queue.get(),
      io_task_queue.get(), &factory, path);
  consumer.Start();
  task_queue->Run();
  io_task_queue->PostQuitTask();
  io_thread.join();
}

int wmain(int argc, wchar_t* argv[]) {
  std::string search_text(argv[2], argv[2] + wcslen(argv[2]));
  Test(argv[1], search_text);
  return 0;
}
