#include "stdafx.h"

#include "io_buffer.h"

IOBuffer::IOBuffer(size_t size) : data_(new uint8_t[size]) {
}

IOBuffer::~IOBuffer() {
  delete[] data_;
}
