#pragma once

#include "file_id.h"

class DataConsumer {
 public:
  virtual ~DataConsumer() {}
  virtual void OnDataConsumerFileOpened(FileId file_id,
                                        const std::wstring& file_path) = 0;
  virtual void OnDataConsumerBlockRead(FileId file_id,
                                       const std::wstring& file_path,
                                       uint8_t* data,
                                       uint32_t size) = 0;
  virtual void OnDataConsumerFileClosed(FileId file_id,
                                        const std::wstring& file_path) = 0;
  virtual void OnDataConsumerFinished() = 0;
};
