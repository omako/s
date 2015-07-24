// ConsoleApplication1.cpp : Defines the entry point for the console application.
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

void test_producer(const std::vector<Worker>& workers,
                   TaskQueue* reply_queue) {
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
  return 0;
}

