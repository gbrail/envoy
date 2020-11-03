#pragma once

#include <memory>

#include "envoy/extensions/filters/http/ext_proc/v3alpha/ext_proc.pb.h"
#include "envoy/grpc/async_client.h"
#include "envoy/http/filter.h"
#include "envoy/http/header_map.h"

#include "common/common/logger.h"

#include "extensions/filters/http/ext_proc/client.h"
#include "extensions/filters/http/common/pass_through_filter.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace ExternalProcessing {

class FilterConfig {
public:
  FilterConfig(const envoy::extensions::filters::http::ext_proc::v3alpha::ExternalProcessor& config)
      : failure_mode_allow_(config.failure_mode_allow()) {}

  bool failureModeAllow() const { return failure_mode_allow_; }

private:
  const bool failure_mode_allow_;
};

using FilterConfigSharedPtr = std::shared_ptr<FilterConfig>;

enum FilterState {
  Idle = 0,
  RequestHeaders,
  RequestBody,
  RequestTrailers,
  ResponseHeaders,
  ResponseBody,
  ResponseTrailers,
};

class Filter : public Logger::Loggable<Logger::Id::filter>,
               public Http::PassThroughFilter,
               public ExternalProcessorCallbacks {
public:
  Filter(const FilterConfigSharedPtr& config, std::unique_ptr<ExternalProcessorClient>&& client);
  ~Filter() override = default;

  // StreamFilter callbacks
  void onDestroy() override;

  Http::FilterHeadersStatus decodeHeaders(Http::RequestHeaderMap& headers,
                                          bool end_stream) override;

  void setDecoderFilterCallbacks(Http::StreamDecoderFilterCallbacks& callbacks) override;

  // ExternalProcessorCallbacks
  void onReceiveMessage(
      std::unique_ptr<envoy::service::ext_proc::v3alpha::ProcessingResponse>&& response) override;
  void onGrpcError(Grpc::Status::GrpcStatus error) override;
  void onGrpcClose() override;

private:
  void handleHeaderMutation(const envoy::service::ext_proc::v3alpha::HeaderMutation& mutation,
    Http::HeaderMap* map);

  FilterConfigSharedPtr config_;
  std::unique_ptr<ExternalProcessorClient> client_;
  Http::StreamDecoderFilterCallbacks* decoder_callbacks_;

  std::unique_ptr<ExternalProcessorStream> stream_;

  FilterState state_ = FilterState::Idle;
  Http::HeaderMap* saved_headers_ = nullptr;
};

} // namespace ExternalProcessing
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy