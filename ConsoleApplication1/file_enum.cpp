#include "stdafx.h"

#include "file_enum.h"

FileEnum::FileEnum(const std::wstring& root_path) {
  DirStackItem item;
  item.dir_path = root_path;
  item.find_handle = INVALID_HANDLE_VALUE;
  dir_stack_.push(item);
}

FileEnum::~FileEnum() {
  while (!dir_stack_.empty()) {
    DirStackItem& item = dir_stack_.top();
    BOOL res = FindClose(item.find_handle);
    assert(res);
    dir_stack_.pop();
  }
}

bool FileEnum::Next() {
  if (dir_stack_.empty())
    return false;
  DirStackItem& item = dir_stack_.top();
  if (item.find_handle == INVALID_HANDLE_VALUE) {
    std::wstring find_path(item.dir_path);
    find_path += L"\\*";
    item.find_handle = FindFirstFile(find_path.c_str(), &find_data_);
    if (item.find_handle == INVALID_HANDLE_VALUE) {
      dir_stack_.pop();
      return Next();
    }
  } else {
    if (!FindNextFile(item.find_handle, &find_data_)) {
      BOOL res = FindClose(item.find_handle);
      assert(res);
      dir_stack_.pop();
      return Next();
    }
  }
  if (wcscmp(find_data_.cFileName, L".") == 0 ||
      wcscmp(find_data_.cFileName, L"..") == 0)
    return Next();
  current_path_ = item.dir_path + L"\\" + find_data_.cFileName;
  if (find_data_.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
    DirStackItem item;
    item.dir_path = current_path_;
    item.find_handle = INVALID_HANDLE_VALUE;
    dir_stack_.push(item);
  }
  return true;
}
