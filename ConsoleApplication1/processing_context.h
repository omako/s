#pragma once

using DataBlock = std::vector<uint8_t>;

class ProcessingContext {
 public:
  virtual ~ProcessingContext() {}
  virtual void ProcessDataBlockPhase1(
      std::shared_ptr<DataBlock> data_block) = 0;
  virtual void MarkEndOfData() = 0;
  virtual void ProcessDataBlockPhase2() = 0;
};

class ProcessingContextFactory {
 public:
  virtual ~ProcessingContextFactory() {}
  virtual ProcessingContext* CreateProcessingContext(
      const std::wstring& file_path) = 0;
};
