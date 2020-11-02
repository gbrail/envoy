#include "extensions/filters/http/ext_proc/ext_proc.h"

#include "extensions/filters/http/ext_proc/headers.h"

using envoy::service::ext_proc::v3alpha::ProcessingRequest;
using envoy::service::ext_proc::v3alpha::ProcessingResponse;

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace ExternalProcessing {

Filter::Filter(const FilterConfigSharedPtr& config, std::unique_ptr<ExternalProcessorClient>&& client)
      : config_(config), client_(std::move(client)) {}

void Filter::onDestroy() {
  if (stream_) {
    stream_->close();
  }
}

Http::FilterHeadersStatus Filter::decodeHeaders(Http::RequestHeaderMap& headers, bool end_of_stream) {
  ENVOY_STREAM_LOG(debug, "Starting gRPC stream", *decoder_callbacks_);
  stream_ = client_->start(*this);

  ProcessingRequest request;
  request.set_response_required(true);
  auto req_hdrs = request.mutable_request_headers();
  req_hdrs->set_end_of_stream(end_of_stream);
  envoyHeadersToProto(headers, req_hdrs);
  // TODO properties and client info

  // TODO stream might have already been closed
  // TODO save state so that we know what to do.
  stream_->send(std::move(request), false);

  return Http::FilterHeadersStatus::Continue;
}

void Filter::setDecoderFilterCallbacks(Http::StreamDecoderFilterCallbacks& callbacks) {
  decoder_callbacks_ = &callbacks;
}

void Filter::onReceiveMessage(std::unique_ptr<ProcessingResponse>&&) {
  // TODO obviously
}

void Filter::onGrpcError(Grpc::Status::GrpcStatus status) {
  ENVOY_LOG(error, "Error from gRPC stream: {}", status);
  stream_.reset(nullptr);
}

void Filter::onGrpcClose() {
  ENVOY_LOG(debug, "gRPC stream closed cleanly");
  stream_.reset(nullptr);
}

} // namespace ExternalProcessing
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy