#include "stdafx.h"

#include "processing_manager.h"

#include "task.h"

namespace {
const unsigned kPrefetchBlockCount = 10;
}  // namespace

ProcessingManager::ProcessingManager(
    unsigned max_open_files,
    unsigned max_parallel_reads,
    unsigned num_processors,
    TaskQueue* task_queue,
    TaskQueue* io_task_queue,
    ProcessingContextFactory* processing_context_factory,
    const std::wstring& path)
    : num_processors_(num_processors),
      task_queue_(task_queue),
      processing_context_factory_(processing_context_factory),
      file_enum_(path),
      data_provider_(max_open_files,
                     max_parallel_reads,
                     &file_enum_,
                     task_queue,
                     io_task_queue,
                     this),
      data_provider_has_finished_(false),
      total_data_size_(0) {
}

ProcessingManager::~ProcessingManager() {
  assert(total_data_size_ == 0);
}

void ProcessingManager::Start() {
  for (unsigned i = 0; i < num_processors_; ++i) {
    processors_.emplace_back(new Processor());
    Processor* processor = processors_.back().get();
    processor->is_busy = false;
    processor->task_queue.reset(new TaskQueueImpl());
    processor->file_id = kInvalidFileId;
    processor->thread =
        std::thread([processor] { processor->task_queue->Run(); });
  }
  data_provider_.Start();
}

void ProcessingManager::OnDataConsumerFileOpened(
    FileId file_id,
    const std::wstring& file_path) {
  File* file =
      files_.insert(Files::value_type(file_id, std::make_unique<File>()))
          .first->second.get();
  file->path = file_path;
  file->is_closed = false;
  processing_queue_.push_back(file_id);
}

void ProcessingManager::OnDataConsumerBlockRead(FileId file_id,
                                                const std::wstring& file_path,
                                                uint8_t* data,
                                                uint32_t size) {
  files_[file_id]->data_blocks.emplace_back(new DataBlock(data, data + size));
  GiveTasksToProcessors();
  total_data_size_ += size;
  if (total_data_size_ >= GetTotalDataSizeLimit())
    data_provider_.Suspend();
}

void ProcessingManager::OnDataConsumerFileClosed(
    FileId file_id,
    const std::wstring& file_path) {
  files_[file_id]->is_closed = true;
  GiveTasksToProcessors();
}

void ProcessingManager::OnDataConsumerFinished() {
  data_provider_has_finished_ = true;
  CheckForEndOfProcessing();
}

void ProcessingManager::Process(Processor* processor,
                                std::shared_ptr<DataBlock> data_block,
                                ProcessingContext* context) {
  context->ProcessDataBlockPhase1(data_block);
  task_queue_->Post(std::unique_ptr<Task>(
      new SimpleTask(std::bind(&ProcessingManager::OnProcessingReply, this,
                               processor, data_block, context))));
}

void ProcessingManager::OnProcessingReply(Processor* processor,
                                          std::shared_ptr<DataBlock> data_block,
                                          ProcessingContext* context) {
  processor->is_busy = false;
  context->ProcessDataBlockPhase2();
  GiveTasksToProcessors();
}

FileId ProcessingManager::ProcessNextFile() {
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
      file->context.reset(
          processing_context_factory_->CreateProcessingContext(file->path));
    return file_id;
  }
  return kInvalidFileId;
}

void ProcessingManager::GiveTasksToProcessors() {
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
          processing_queue_.push_front(processor->file_id);
        processor->file_id = kInvalidFileId;
        file_id = ProcessNextFile();
      } else {
        file_id = processor->file_id;
      }
    }
    if (file_id == kInvalidFileId)
      continue;
    File* file = files_[file_id].get();
    std::shared_ptr<DataBlock> data_block(file->data_blocks.front());
    file->data_blocks.pop_front();
    total_data_size_ -= data_block->size();
    processor->file_id = file_id;
    processor->is_busy = true;
    processor->task_queue->Post(std::unique_ptr<Task>(new SimpleTask(
        std::bind(&ProcessingManager::Process, this, processor.get(),
                  data_block, file->context.get()))));
  }
  CheckForEndOfProcessing();
  if (total_data_size_ < GetTotalDataSizeLimit())
    data_provider_.Resume();
}

void ProcessingManager::CheckForEndOfProcessing() {
  if (!data_provider_has_finished_)
    return;
  if (!files_.empty())
    return;
  int idle_count = 0;
  for (const std::unique_ptr<Processor>& processor : processors_)
    if (!processor->is_busy)
      ++idle_count;
  if (idle_count != num_processors_)
    return;
  for (unsigned i = 0; i < num_processors_; ++i)
    processors_[i]->task_queue->PostQuitTask();
  for (unsigned i = 0; i < num_processors_; ++i)
    processors_[i]->thread.join();
  task_queue_->PostQuitTask();
}

uint64_t ProcessingManager::GetTotalDataSizeLimit() const {
  return num_processors_ * DataProvider::kBlockSize * kPrefetchBlockCount;
}
