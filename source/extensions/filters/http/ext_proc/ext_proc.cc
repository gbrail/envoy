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
  // TODO check if we are disabled.

  ENVOY_STREAM_LOG(debug, "Starting gRPC stream", *decoder_callbacks_);
  stream_ = client_->start(*this);

  ProcessingRequest request;
  request.set_response_required(true);
  auto req_hdrs = request.mutable_request_headers();
  req_hdrs->set_end_of_stream(end_of_stream);
  envoyHeadersToProto(headers, req_hdrs);
  // TODO properties and client info

  // TODO stream might have already been closed
  
  saved_headers_ = &headers;
  state_ = FilterState::RequestHeaders;
  stream_->send(std::move(request), false);

  return Http::FilterHeadersStatus::StopAllIterationAndWatermark;
}

void Filter::setDecoderFilterCallbacks(Http::StreamDecoderFilterCallbacks& callbacks) {
  decoder_callbacks_ = &callbacks;
}

void Filter::onReceiveMessage(std::unique_ptr<ProcessingResponse>&& response) {
  // TODO obviously do something with the message
  if (state_ == FilterState::RequestHeaders) {
    if (response->has_request_headers()) {
      if (response->request_headers().has_header_mutation()) {
        handleHeaderMutation(response->request_headers().header_mutation(), saved_headers_);
      }
      // TODO body mutation
    } else {
      ENVOY_STREAM_LOG(warn, "Mismatched remote filter response: Expected request headers",
        *decoder_callbacks_);
    }
    decoder_callbacks_->continueDecoding();
  }
}

void Filter::onGrpcError(Grpc::Status::GrpcStatus status) {
  ENVOY_LOG(error, "Error from gRPC stream: {}", status);
  stream_.reset(nullptr);
}

void Filter::onGrpcClose() {
  ENVOY_LOG(debug, "gRPC stream closed cleanly");
  stream_.reset(nullptr);
}

void Filter::handleHeaderMutation(const envoy::service::ext_proc::v3alpha::HeaderMutation& mutation,
    Http::HeaderMap* map) {
  for (auto setter = mutation.set_headers().cbegin();
    setter != mutation.set_headers().cend();
    setter++) {
    if (setter->append().value()) {
      // TODO do we want addCopy or appendCopy here?
      map->addCopy(Http::LowerCaseString(setter->header().key()), setter->header().value());
    } else {
      map->setCopy(Http::LowerCaseString(setter->header().key()), setter->header().value());
    }
  }
  for (auto remove = mutation.remove_headers().cbegin();
    remove != mutation.remove_headers().cend();
    remove++) {
    map->remove(Http::LowerCaseString(*remove));
  }
}

} // namespace ExternalProcessing
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy