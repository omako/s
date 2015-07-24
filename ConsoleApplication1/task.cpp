#include "stdafx.h"

#include "task.h"

namespace {

const int kMaxIOQueueLength = 1000;

class QuitTask : public Task {
 public:
  QuitTask(TaskQueue* task_queue);

 private:
  // Task
  void Execute() override;

  TaskQueue* task_queue_;
};

QuitTask::QuitTask(TaskQueue* task_queue) : task_queue_(task_queue) {
}

void QuitTask::Execute() {
  task_queue_->Stop();
}

}  // namespace

void ThreadChecker::AssertSameThread() {
  if (thread_id_ == std::thread::id())
    thread_id_ = std::this_thread::get_id();
  else
    assert(std::this_thread::get_id() == thread_id_);
}

void TaskQueue::PostQuitTask() {
  Post(std::unique_ptr<Task>(new QuitTask(this)));
}

TaskQueueImpl::TaskQueueImpl() : is_running_(false) {
}

void TaskQueueImpl::Post(std::unique_ptr<Task> task) {
  std::lock_guard<std::mutex> queue_mutex_lock(queue_mutex_);
  task_queue_.push(std::move(task));
  queue_not_empty_cv_.notify_one();
}

void TaskQueueImpl::Run() {
  thread_checker_.AssertSameThread();
  assert(!is_running_);
  is_running_ = true;
  while (is_running_) {
    std::unique_lock<std::mutex> queue_mutex_lock(queue_mutex_);
    queue_not_empty_cv_.wait(queue_mutex_lock,
                             [this] { return !task_queue_.empty(); });
    std::unique_ptr<Task> task(std::move(task_queue_.front()));
    task_queue_.pop();
    queue_mutex_lock.unlock();
    task->Execute();
  }
}

void TaskQueueImpl::Stop() {
  thread_checker_.AssertSameThread();
  is_running_ = false;
}

SimpleTask::SimpleTask(const std::function<void()>& task) : task_(task) {
}

void SimpleTask::Execute() {
  task_();
}

IOTaskQueue::IOTaskQueue() : is_running_(false) {
  queue_sem_ = CreateSemaphore(NULL, 0, kMaxIOQueueLength, NULL);
  assert(queue_sem_);
}

IOTaskQueue::~IOTaskQueue() {
}

void IOTaskQueue::Post(std::unique_ptr<Task> task) {
  std::lock_guard<std::mutex> queue_mutex_lock(queue_mutex_);
  task_queue_.push(std::move(task));
  BOOL res = ReleaseSemaphore(queue_sem_, 1, nullptr);
  assert(res);
}

void IOTaskQueue::Run() {
  thread_checker_.AssertSameThread();
  assert(!is_running_);
  is_running_ = true;
  while (is_running_) {
    DWORD wait_res = WaitForSingleObjectEx(queue_sem_, INFINITE, TRUE);
    if (wait_res == WAIT_OBJECT_0) {
      std::unique_lock<std::mutex> queue_mutex_lock(queue_mutex_);
      std::unique_ptr<Task> task(std::move(task_queue_.front()));
      task_queue_.pop();
      queue_mutex_lock.unlock();
      task->Execute();
    }
  }
}

void IOTaskQueue::Stop() {
  thread_checker_.AssertSameThread();
  is_running_ = false;
}
