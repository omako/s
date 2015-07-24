#include "stdafx.h"

#include "file_reader_proxy.h"
#include "io_buffer.h"
#include "task.h"

class FileReaderProxyTest : public ::testing::Test {
 public:
  FileReaderProxyTest()
      : task_queue_(new TaskQueueImpl()),
        io_task_queue_(new IOTaskQueue()),
        file_(task_queue_.get(), io_task_queue_.get()) {
    io_thread_ = std::thread(std::bind(&FileReaderProxyTest::IOThread, this));
  }

  ~FileReaderProxyTest() {
    io_task_queue_->PostQuitTask();
    io_thread_.join();
  }

 protected:
  bool Open(const std::wstring& file_name) {
    task_queue_->Post(std::unique_ptr<Task>(new SimpleTask(
        std::bind(&FileReaderProxyTest::OpenFile, this, file_name))));
    task_queue_->Run();
    return open_result_;
  }

  FileReader::ReadResult Read(uint64_t offset, uint32_t size) {
    task_queue_->Post(std::unique_ptr<Task>(new SimpleTask(
        std::bind(&FileReaderProxyTest::ReadFile, this, offset, size))));
    task_queue_->Run();
    return read_result_;
  }

  bool Close() {
    task_queue_->Post(std::unique_ptr<Task>(
        new SimpleTask(std::bind(&FileReaderProxyTest::CloseFile, this))));
    task_queue_->Run();
    return open_result_;
  }

 private:
  void IOThread() { io_task_queue_->Run(); }

  void OpenFile(const std::wstring& file_name) {
    file_.Open(file_name, std::bind(&FileReaderProxyTest::OpenCallback, this,
                                    std::placeholders::_1));
  }

  void OpenCallback(bool result) {
    open_result_ = result;
    task_queue_->PostQuitTask();
  }

  void ReadFile(uint64_t offset, uint32_t size) {
    file_.Read(offset, size, std::bind(&FileReaderProxyTest::ReadCallback, this,
                                       std::placeholders::_1));
  }

  void ReadCallback(FileReader::ReadResult read_result) {
    read_result_ = read_result;
    task_queue_->PostQuitTask();
  }

  void CloseFile() {
    file_.Close(std::bind(&FileReaderProxyTest::CloseCallback, this));
  }

  void CloseCallback() { task_queue_->PostQuitTask(); }

  std::unique_ptr<TaskQueue> task_queue_;
  std::unique_ptr<TaskQueue> io_task_queue_;
  std::thread io_thread_;
  FileReaderProxy file_;
  bool open_result_;
  FileReader::ReadResult read_result_;
};

class TestFile {
 public:
  TestFile() {
    wchar_t temp_dir[MAX_PATH];
    DWORD res = GetTempPathW(MAX_PATH, temp_dir);
    assert(res > 0 && res < MAX_PATH);
    wchar_t temp_file_path[MAX_PATH];
    UINT res2 = GetTempFileNameW(temp_dir, L"", 0, temp_file_path);
    assert(res2 != 0);
    file_path_ = temp_file_path;
    Write("");
  }

  ~TestFile() {
    if (!file_path_.empty())
      DeleteFileW(file_path_.c_str());
  }

  void Write(const std::string& data) {
    std::ofstream st(file_path_);
    st << data;
  }

  std::wstring file_path() const { return file_path_; }

 private:
  std::wstring file_path_;
};

TEST_F(FileReaderProxyTest, BadFileName) {
  EXPECT_FALSE(Open(L""));
}

TEST_F(FileReaderProxyTest, EmptyFile) {
  TestFile test_file;
  test_file.Write("");
  EXPECT_TRUE(Open(test_file.file_path()));
  FileReader::ReadResult read_result = Read(0, 10);
  EXPECT_EQ(FileReader::FILE_READ_EOF, read_result.status);
  Close();
}

TEST_F(FileReaderProxyTest, NormalRead) {
  TestFile test_file;
  test_file.Write("123");
  EXPECT_TRUE(Open(test_file.file_path()));
  FileReader::ReadResult read_result = Read(1, 2);
  EXPECT_EQ(FileReader::FILE_READ_SUCCESS, read_result.status);
  EXPECT_EQ(2, read_result.size);
  EXPECT_EQ('2', read_result.buffer->data()[0]);
  EXPECT_EQ('3', read_result.buffer->data()[1]);
  Close();
}

TEST_F(FileReaderProxyTest, ReadMoreThanAvailable) {
  TestFile test_file;
  test_file.Write("1");
  EXPECT_TRUE(Open(test_file.file_path()));
  FileReader::ReadResult read_result = Read(0, 10);
  EXPECT_EQ(FileReader::FILE_READ_SUCCESS, read_result.status);
  EXPECT_EQ(1, read_result.size);
  EXPECT_EQ('1', read_result.buffer->data()[0]);
  Close();
}

TEST_F(FileReaderProxyTest, ReadBeyondEOF) {
  TestFile test_file;
  test_file.Write("1");
  EXPECT_TRUE(Open(test_file.file_path()));
  FileReader::ReadResult read_result = Read(10, 10);
  EXPECT_EQ(FileReader::FILE_READ_EOF, read_result.status);
  Close();
}

class MultipleFileReadersTest : public ::testing::Test {
 public:
  MultipleFileReadersTest()
      : task_queue_(new TaskQueueImpl()),
        io_task_queue_(new IOTaskQueue()),
        rnd_gen(rnd_seed()),
        closed_file_count_(0) {
    io_thread_ =
        std::thread(std::bind(&MultipleFileReadersTest::IOThread, this));
  }

  ~MultipleFileReadersTest() {
    io_task_queue_->PostQuitTask();
    io_thread_.join();
  }

 protected:
  void Run() {
    for (int i = 0; i < kReaderCount; ++i) {
      tests_.emplace_back(new FileReaderTest());
      FileReaderTest* test = tests_.back().get();
      test->file_reader.reset(
          new FileReaderProxy(task_queue_.get(), io_task_queue_.get()));
      test->test_file_contents = gen_random_string(kTestFileSize);
      test->test_file.Write(test->test_file_contents);
      test->iteration = 0;
      test->current_read_offset = 0;
      test->current_read_size = 0;
    }
    for (size_t i = 0; i < tests_.size(); ++i) {
      tests_[i]->file_reader->Open(
          tests_[i]->test_file.file_path(),
          std::bind(&MultipleFileReadersTest::OpenCallback, this, i,
                    std::placeholders::_1));
    }
    task_queue_->Run();
  }

 private:
  static const int kReaderCount = 10;
  static const int kIterationCount = 1000;
  static const int kTestFileSize = 1000;

  std::string gen_random_string(int len) {
    std::string result;
    std::uniform_int_distribution<int> dist('a', 'z');
    for (int i = 0; i < len; ++i)
      result += dist(rnd_gen);
    return result;
  }

  void OpenCallback(int test_index, bool result) {
    ASSERT_TRUE(result);
    FileReaderTest* test = tests_[test_index].get();
    NextIteration(test_index);
  }

  void ReadCallback(int test_index, FileReader::ReadResult read_result) {
    ASSERT_TRUE(read_result.status == FileReader::FILE_READ_SUCCESS);
    FileReaderTest* test = tests_[test_index].get();
    ASSERT_EQ(read_result.size, test->current_read_size);
    ASSERT_TRUE(
        memcmp(test->test_file_contents.data() + test->current_read_offset,
               read_result.buffer->data(), test->current_read_size) == 0);
    NextIteration(test_index);
  }

  void NextIteration(int test_index) {
    FileReaderTest* test = tests_[test_index].get();
    if (test->iteration == kIterationCount) {
      test->file_reader->Close(
          std::bind(&MultipleFileReadersTest::CloseCallback, this));
      return;
    }
    ++test->iteration;
    test->current_read_offset =
        std::uniform_int_distribution<uint64_t>(0, kTestFileSize - 1)(rnd_gen);
    test->current_read_size =
        std::uniform_int_distribution<uint32_t>(1, kTestFileSize)(rnd_gen);
    if (test->current_read_offset + test->current_read_size > kTestFileSize)
      test->current_read_size =
          static_cast<uint32_t>(kTestFileSize - test->current_read_offset);
    test->file_reader->Read(test->current_read_offset, test->current_read_size,
                            std::bind(&MultipleFileReadersTest::ReadCallback,
                                      this, test_index, std::placeholders::_1));
  }

  void CloseCallback() {
    ++closed_file_count_;
    if (closed_file_count_ == kReaderCount)
      task_queue_->PostQuitTask();
  }

  void IOThread() { io_task_queue_->Run(); }

  struct FileReaderTest {
    TestFile test_file;
    std::string test_file_contents;
    std::unique_ptr<FileReaderProxy> file_reader;
    int iteration;
    uint64_t current_read_offset;
    uint32_t current_read_size;
  };

  std::unique_ptr<TaskQueue> task_queue_;
  std::unique_ptr<TaskQueue> io_task_queue_;
  std::thread io_thread_;
  std::vector<std::unique_ptr<FileReaderTest>> tests_;
  std::random_device rnd_seed;
  std::mt19937 rnd_gen;
  int closed_file_count_;
};

TEST_F(MultipleFileReadersTest, Test) {
  Run();
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
