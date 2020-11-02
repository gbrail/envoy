#include "extensions/filters/http/ext_proc/headers.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace ExternalProcessing {

void envoyHeadersToProto(Http::HeaderMap& headers,
                       envoy::service::ext_proc::v3alpha::HttpHeaders* headersOut) {
  auto out = headersOut->mutable_headers();
  headers.iterate([&out](const Http::HeaderEntry& hdr) -> Http::HeaderMap::Iterate {
    auto new_hdr = out->add_headers();
    new_hdr->set_key(hdr.key().getStringView().data(), hdr.key().size());
    new_hdr->set_value(hdr.value().getStringView().data(), hdr.value().size());
    return Http::HeaderMap::Iterate::Continue;
  });
}

} // namespace ExternalProcessing
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy