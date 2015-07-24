#pragma once

class IOBuffer {
 public:
  IOBuffer(size_t size);
  ~IOBuffer();
  uint8_t* data() const { return data_; }

 private:
  uint8_t* data_;
};
