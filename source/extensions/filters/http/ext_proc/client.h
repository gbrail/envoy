#pragma once

#include <memory>

#include "envoy/grpc/status.h"
#include "envoy/service/ext_proc/v3alpha/external_processor.pb.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace ExternalProcessing {

class ExternalProcessorStream {
public:
  virtual ~ExternalProcessorStream() = default;
  virtual void send(envoy::service::ext_proc::v3alpha::ProcessingRequest&& request,
                    bool end_stream) = 0;
  virtual void close() = 0;
};

using ExternalProcessorStreamPtr = std::unique_ptr<ExternalProcessorStream>;

class ExternalProcessorCallbacks {
public:
  virtual ~ExternalProcessorCallbacks() = default;
  virtual void onReceiveMessage(
      std::unique_ptr<envoy::service::ext_proc::v3alpha::ProcessingResponse>&& response) = 0;
  virtual void onGrpcError(Grpc::Status::GrpcStatus error) = 0;
  virtual void onGrpcClose() = 0;
};

class ExternalProcessorClient {
public:
  virtual ~ExternalProcessorClient() = default;
  virtual ExternalProcessorStreamPtr start(ExternalProcessorCallbacks& callbacks) = 0;
};

} // namespace ExternalProcessing
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy