#pragma once

#include "extensions/filters/http/ext_proc/client.h"

#include "gmock/gmock.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace ExternalProcessing {

class MockCallbacks : public ExternalProcessorCallbacks {
public:
  MockCallbacks();
  virtual ~MockCallbacks();
  MOCK_METHOD(void, onReceiveMessage,
              (std::unique_ptr<envoy::service::ext_proc::v3alpha::ProcessingResponse> && message));
  MOCK_METHOD(void, onGrpcError, (Grpc::Status::GrpcStatus error));
  MOCK_METHOD(void, onGrpcClose, ());
};

class MockStream : public ExternalProcessorStream {
public:
  MockStream();
  virtual ~MockStream();
  MOCK_METHOD(void, send,
              (envoy::service::ext_proc::v3alpha::ProcessingRequest && request, bool end_stream));
  MOCK_METHOD(void, close, ());
};

class MockClient : public ExternalProcessorClient {
public:
  MockClient();
  virtual ~MockClient();
  MOCK_METHOD(ExternalProcessorStreamPtr, start, (ExternalProcessorCallbacks & callbacks));
};

} // namespace ExternalProcessing
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy