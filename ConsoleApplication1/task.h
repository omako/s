#pragma once

class ThreadChecker {
 public:
  void AssertSameThread();

 private:
  std::thread::id thread_id_;
};

class Task {
 public:
  virtual ~Task() {}
  virtual void Execute() = 0;
};

class TaskQueue {
 public:
  virtual ~TaskQueue() {}
  virtual void Post(std::unique_ptr<Task> task) = 0;
  virtual void Run() = 0;
  virtual void Stop() = 0;
  void PostQuitTask();
};

class TaskQueueImpl : public TaskQueue {
 public:
  TaskQueueImpl();

 private:
  // TaskQueue
  void Post(std::unique_ptr<Task> task) override;
  void Run() override;
  void Stop() override;

  TaskQueueImpl(const TaskQueueImpl&) = delete;
  TaskQueueImpl& operator=(const TaskQueueImpl&) = delete;

  std::queue<std::unique_ptr<Task>> task_queue_;
  std::condition_variable queue_not_empty_cv_;
  std::mutex queue_mutex_;
  bool is_running_;
  ThreadChecker thread_checker_;
};

class SimpleTask : public Task {
 public:
  SimpleTask(const std::function<void()>& task);
 
 private:
  // Task
  void Execute() override;

  std::function<void()> task_;
};

template <class ReplyType>
class TaskWithReply : public Task {
 public:
  TaskWithReply(const std::function<ReplyType()>& task,
                const std::function<void(const ReplyType&)>& reply,
                TaskQueue* reply_queue)
      : task_(task), reply_(reply), reply_queue_(reply_queue) {}

 private:
  class ReplyTask : public Task {
   public:
    ReplyTask(const std::function<void(const ReplyType&)> reply,
              const ReplyType& reply_value)
        : reply_(reply), reply_value_(reply_value) {}

   private:
    // Task
    void Execute() override { reply_(reply_value_); }

    std::function<void(const ReplyType&)> reply_;
    ReplyType reply_value_;
  };

  // Task
  void Execute() override {
    ReplyType reply_value = task_();
    reply_queue_->Post(
        std::unique_ptr<Task>(new ReplyTask(reply_, reply_value)));
  }
  
  const std::function<ReplyType()> task_;
  const std::function<void(const ReplyType&)> reply_;
  TaskQueue* reply_queue_;
};

class IOTaskQueue : public TaskQueue {
 public:
  IOTaskQueue();
  ~IOTaskQueue();

  // TaskQueue
  void Post(std::unique_ptr<Task> task) override;
  void Run() override;
  void Stop() override;

 private:
  IOTaskQueue(const IOTaskQueue&) = delete;
  IOTaskQueue& operator=(const IOTaskQueue&) = delete;

  std::queue<std::unique_ptr<Task>> task_queue_;
  std::mutex queue_mutex_;
  HANDLE queue_sem_;
  bool is_running_;
  ThreadChecker thread_checker_;
};
