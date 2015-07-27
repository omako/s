// ConsoleApplication1.cpp : Defines the entry point for the console
// application.
//

#include "stdafx.h"

#include "task.h"
#include "processing_context.h"
#include "processing_manager.h"

class Report {
 public:
  void ReportFound(const std::wstring& file_path, uint64_t file_offset);
};

void Report::ReportFound(const std::wstring& file_path, uint64_t file_offset) {
  std::wcout << file_path << " @ " << file_offset << std::endl;
}

class ProcessingContextImpl : public ProcessingContext {
 public:
  ProcessingContextImpl(const std::string& search_text,
                        Report* report,
                        const std::wstring& file_path);

 private:
  void ProcessDataBlockPhase1(std::shared_ptr<DataBlock> data_block) override;
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
  const size_t last_data_index =
      backup_buffer_.size() + data_block->size() - search_text_.size();
  for (size_t data_index = 0; data_index <= last_data_index; ++data_index) {
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

void ProcessingContextImpl::ProcessDataBlockPhase2() {
  for (uint64_t file_offset : found_)
    report_->ReportFound(file_path_, file_offset);
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

void Test(const std::wstring& path, const std::string& search_text) {
  std::unique_ptr<TaskQueue> task_queue(new TaskQueueImpl());
  std::unique_ptr<TaskQueue> io_task_queue(new IOTaskQueue());
  std::thread io_thread([&io_task_queue] { io_task_queue->Run(); });
  Report report;
  ProcessingContextFactoryImpl factory(search_text, &report);
  ProcessingManager consumer(std::thread::hardware_concurrency(), 1,
                             std::thread::hardware_concurrency(),
                             task_queue.get(), io_task_queue.get(),
                             &factory, path);
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
