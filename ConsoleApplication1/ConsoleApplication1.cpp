// ConsoleApplication1.cpp : Defines the entry point for the console
// application.
//

#include "stdafx.h"

#include "task.h"
#include "processing_context.h"
#include "processing_manager.h"

class ProcessingContextImpl : public ProcessingContext {
 private:
  void ProcessDataBlockPhase1(std::shared_ptr<DataBlock> data_block) override;
  void ProcessDataBlockPhase2() override;
};

void ProcessingContextImpl::ProcessDataBlockPhase1(
    std::shared_ptr<DataBlock> data_block) {
}

void ProcessingContextImpl::ProcessDataBlockPhase2() {
}

void Test(const std::wstring& path) {
  std::unique_ptr<TaskQueue> task_queue(new TaskQueueImpl());
  std::unique_ptr<TaskQueue> io_task_queue(new IOTaskQueue());
  std::thread io_thread([&io_task_queue] { io_task_queue->Run(); });
  ProcessingManager consumer(4, 1, 4, task_queue.get(), io_task_queue.get(),
                             [] { return new ProcessingContextImpl(); }, path);
  consumer.Start();
  task_queue->Run();
  io_task_queue->PostQuitTask();
  io_thread.join();
}

int wmain(int argc, wchar_t* argv[]) {
  Test(argv[1]);
  return 0;
}
