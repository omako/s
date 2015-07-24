#pragma once

class FileEnum {
 public:
  FileEnum(const std::wstring& root_path);
  ~FileEnum();
  bool Next();
  const std::wstring& current_path() const { return current_path_; }
  const WIN32_FIND_DATA& find_data() const { return find_data_; }
  bool IsDir() const {
    return !!(find_data_.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
  }

 private:
  struct DirStackItem {
    HANDLE find_handle;
    std::wstring dir_path;
  };

  std::stack<DirStackItem> dir_stack_;
  std::wstring current_path_;
  WIN32_FIND_DATA find_data_;
};
