#pragma once

#include "data_consumer.h"
#include "data_provider.h"
#include "file_enum.h"
#include "processing_context.h"

class TaskQueue;

class ProcessingManager : public DataConsumer {
 public:
  ProcessingManager(unsigned max_open_files,
                    unsigned max_parallel_reads,
                    unsigned num_processors,
                    TaskQueue* task_queue,
                    TaskQueue* io_task_queue,
                    const ProcessingContextFactory& processing_context_factory,
                    const std::wstring& path);

  void Start();

 private:
  struct Processor {
    std::thread thread;
    std::unique_ptr<TaskQueue> task_queue;
    bool is_busy;
    FileId file_id;
  };
  struct File {
    std::wstring path;
    bool is_closed;
    std::list<std::shared_ptr<DataBlock>> data_blocks;
    std::unique_ptr<ProcessingContext> context;
  };
  using Files = std::unordered_map<FileId, std::unique_ptr<File>>;
  using ProcessingQueue = std::deque<FileId>;

  ProcessingManager(const ProcessingManager&) = delete;
  ProcessingManager& operator=(const ProcessingManager&) = delete;

  // DataConsumer
  void OnDataConsumerFileOpened(FileId file_id,
                                const std::wstring& file_path) override;
  void OnDataConsumerBlockRead(FileId file_id,
                               const std::wstring& file_path,
                               uint8_t* data,
                               uint32_t size) override;
  void OnDataConsumerFileClosed(FileId file_id,
                                const std::wstring& file_path) override;
  void OnDataConsumerFinished() override;

  void Process(Processor* processor,
               std::shared_ptr<DataBlock> data_block,
               ProcessingContext* context);
  void OnProcessingReply(Processor* processor, ProcessingContext* context);
  FileId ProcessNextFile();
  void GiveTasksToProcessors();
  void CheckForEndOfProcessing();

  const unsigned num_processors_;
  TaskQueue* task_queue_;
  TaskQueue* io_task_queue_;
  ProcessingContextFactory processing_context_factory_;
  FileEnum file_enum_;
  DataProvider data_provider_;
  bool data_provider_has_finished_;
  Files files_;
  std::unordered_set<FileId> files_in_processing_;
  ProcessingQueue processing_queue_;
  std::vector<std::unique_ptr<Processor>> processors_;
};
