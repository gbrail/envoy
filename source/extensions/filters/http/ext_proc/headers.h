#pragma once

#include "envoy/http/header_map.h"
#include "envoy/service/ext_proc/v3alpha/external_processor.pb.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace ExternalProcessing {

extern void envoyHeadersToProto(Http::HeaderMap& headers,
                                envoy::service::ext_proc::v3alpha::HttpHeaders* headersOut);

} // namespace ExternalProcessing
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
