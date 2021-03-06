#include "common/config/new_grpc_mux_impl.h"

#include "envoy/service/discovery/v3/discovery.pb.h"

#include "common/common/assert.h"
#include "common/common/backoff_strategy.h"
#include "common/common/token_bucket_impl.h"
#include "common/config/utility.h"
#include "common/config/version_converter.h"
#include "common/config/xds_context_params.h"
#include "common/config/xds_resource.h"
#include "common/memory/utils.h"
#include "common/protobuf/protobuf.h"
#include "common/protobuf/utility.h"

namespace Envoy {
namespace Config {

NewGrpcMuxImpl::NewGrpcMuxImpl(Grpc::RawAsyncClientPtr&& async_client,
                               Event::Dispatcher& dispatcher,
                               const Protobuf::MethodDescriptor& service_method,
                               envoy::config::core::v3::ApiVersion transport_api_version,
                               Random::RandomGenerator& random, Stats::Scope& scope,
                               const RateLimitSettings& rate_limit_settings,
                               const LocalInfo::LocalInfo& local_info)
    : grpc_stream_(this, std::move(async_client), service_method, random, dispatcher, scope,
                   rate_limit_settings),
      local_info_(local_info), transport_api_version_(transport_api_version),
      dispatcher_(dispatcher),
      enable_type_url_downgrade_and_upgrade_(Runtime::runtimeFeatureEnabled(
          "envoy.reloadable_features.enable_type_url_downgrade_and_upgrade")) {}

ScopedResume NewGrpcMuxImpl::pause(const std::string& type_url) {
  return pause(std::vector<std::string>{type_url});
}

ScopedResume NewGrpcMuxImpl::pause(const std::vector<std::string> type_urls) {
  for (const auto& type_url : type_urls) {
    pausable_ack_queue_.pause(type_url);
  }

  return std::make_unique<Cleanup>([this, type_urls]() {
    for (const auto& type_url : type_urls) {
      pausable_ack_queue_.resume(type_url);
      if (!pausable_ack_queue_.paused(type_url)) {
        trySendDiscoveryRequests();
      }
    }
  });
}

void NewGrpcMuxImpl::registerVersionedTypeUrl(const std::string& type_url) {

  TypeUrlMap& type_url_map = typeUrlMap();
  if (type_url_map.find(type_url) != type_url_map.end()) {
    return;
  }
  // If type_url is v3, earlier_type_url will contain v2 type url.
  absl::optional<std::string> earlier_type_url = ApiTypeOracle::getEarlierTypeUrl(type_url);
  // Register v2 to v3 and v3 to v2 type_url mapping in the hash map.
  if (earlier_type_url.has_value()) {
    type_url_map[earlier_type_url.value()] = type_url;
    type_url_map[type_url] = earlier_type_url.value();
  }
}

void NewGrpcMuxImpl::onDiscoveryResponse(
    std::unique_ptr<envoy::service::discovery::v3::DeltaDiscoveryResponse>&& message,
    ControlPlaneStats&) {
  ENVOY_LOG(debug, "Received DeltaDiscoveryResponse for {} at version {}", message->type_url(),
            message->system_version_info());
  auto sub = subscriptions_.find(message->type_url());
  // If this type url is not watched, try another version type url.
  if (enable_type_url_downgrade_and_upgrade_ && sub == subscriptions_.end()) {
    const std::string& type_url = message->type_url();
    registerVersionedTypeUrl(type_url);
    TypeUrlMap& type_url_map = typeUrlMap();
    if (type_url_map.find(type_url) != type_url_map.end()) {
      sub = subscriptions_.find(type_url_map[type_url]);
    }
  }
  if (sub == subscriptions_.end()) {
    ENVOY_LOG(warn,
              "Dropping received DeltaDiscoveryResponse (with version {}) for non-existent "
              "subscription {}.",
              message->system_version_info(), message->type_url());
    return;
  }

  kickOffAck(sub->second->sub_state_.handleResponse(*message));
  Memory::Utils::tryShrinkHeap();
}

void NewGrpcMuxImpl::onStreamEstablished() {
  for (auto& [type_url, subscription] : subscriptions_) {
    UNREFERENCED_PARAMETER(type_url);
    subscription->sub_state_.markStreamFresh();
  }
  trySendDiscoveryRequests();
}

void NewGrpcMuxImpl::onEstablishmentFailure() {
  // If this happens while Envoy is still initializing, the onConfigUpdateFailed() we ultimately
  // call on CDS will cause LDS to start up, which adds to subscriptions_ here. So, to avoid a
  // crash, the iteration needs to dance around a little: collect pointers to all
  // SubscriptionStates, call on all those pointers we haven't yet called on, repeat if there are
  // now more SubscriptionStates.
  absl::flat_hash_map<std::string, DeltaSubscriptionState*> all_subscribed;
  absl::flat_hash_map<std::string, DeltaSubscriptionState*> already_called;
  do {
    for (auto& [type_url, subscription] : subscriptions_) {
      all_subscribed[type_url] = &subscription->sub_state_;
    }
    for (auto& sub : all_subscribed) {
      if (already_called.insert(sub).second) { // insert succeeded ==> not already called
        sub.second->handleEstablishmentFailure();
      }
    }
  } while (all_subscribed.size() != subscriptions_.size());
}

void NewGrpcMuxImpl::onWriteable() { trySendDiscoveryRequests(); }

void NewGrpcMuxImpl::kickOffAck(UpdateAck ack) {
  pausable_ack_queue_.push(std::move(ack));
  trySendDiscoveryRequests();
}

// TODO(fredlas) to be removed from the GrpcMux interface very soon.
void NewGrpcMuxImpl::start() { grpc_stream_.establishNewStream(); }

GrpcMuxWatchPtr NewGrpcMuxImpl::addWatch(const std::string& type_url,
                                         const std::set<std::string>& resources,
                                         SubscriptionCallbacks& callbacks,
                                         OpaqueResourceDecoder& resource_decoder,
                                         const bool use_namespace_matching) {
  auto entry = subscriptions_.find(type_url);
  if (entry == subscriptions_.end()) {
    // We don't yet have a subscription for type_url! Make one!
    if (enable_type_url_downgrade_and_upgrade_) {
      registerVersionedTypeUrl(type_url);
    }
    addSubscription(type_url, use_namespace_matching);
    return addWatch(type_url, resources, callbacks, resource_decoder, use_namespace_matching);
  }

  Watch* watch = entry->second->watch_map_.addWatch(callbacks, resource_decoder);
  // updateWatch() queues a discovery request if any of 'resources' are not yet subscribed.
  updateWatch(type_url, watch, resources, use_namespace_matching);
  return std::make_unique<WatchImpl>(type_url, watch, *this);
}

// Updates the list of resource names watched by the given watch. If an added name is new across
// the whole subscription, or if a removed name has no other watch interested in it, then the
// subscription will enqueue and attempt to send an appropriate discovery request.
void NewGrpcMuxImpl::updateWatch(const std::string& type_url, Watch* watch,
                                 const std::set<std::string>& resources,
                                 const bool creating_namespace_watch) {
  ASSERT(watch != nullptr);
  auto sub = subscriptions_.find(type_url);
  RELEASE_ASSERT(sub != subscriptions_.end(),
                 fmt::format("Watch of {} has no subscription to update.", type_url));
  // If this is a glob collection subscription, we need to compute actual context parameters.
  std::set<std::string> xdstp_resources;
  // TODO(htuch): add support for resources beyond glob collections, the constraints below around
  // resource size and ID reflect the progress of the xdstp:// implementation.
  if (!resources.empty() && XdsResourceIdentifier::hasXdsTpScheme(*resources.begin())) {
    // Callers must be asking for a single resource, the collection.
    ASSERT(resources.size() == 1);
    auto resource = XdsResourceIdentifier::decodeUrn(*resources.begin());
    // We only know how to deal with glob collections and static context parameters right now.
    // TODO(htuch): add support for dynamic context params and list collections in the future.
    if (absl::EndsWith(resource.id(), "/*")) {
      auto encoded_context = XdsContextParams::encodeResource(
          local_info_.contextProvider().nodeContext(), resource.context(), {}, {});
      resource.mutable_context()->CopyFrom(encoded_context);
      XdsResourceIdentifier::EncodeOptions encode_options;
      encode_options.sort_context_params_ = true;
      xdstp_resources.insert(XdsResourceIdentifier::encodeUrn(resource, encode_options));
    } else {
      // TODO(htuch): We will handle list collections here in future work.
      NOT_IMPLEMENTED_GCOVR_EXCL_LINE;
    }
  }
  auto added_removed = sub->second->watch_map_.updateWatchInterest(
      watch, xdstp_resources.empty() ? resources : xdstp_resources);
  if (creating_namespace_watch && xdstp_resources.empty()) {
    // This is to prevent sending out of requests that contain prefixes instead of resource names
    sub->second->sub_state_.updateSubscriptionInterest({}, {});
  } else {
    sub->second->sub_state_.updateSubscriptionInterest(added_removed.added_,
                                                       added_removed.removed_);
  }
  // Tell the server about our change in interest, if any.
  if (sub->second->sub_state_.subscriptionUpdatePending()) {
    trySendDiscoveryRequests();
  }
}

void NewGrpcMuxImpl::requestOnDemandUpdate(const std::string& type_url,
                                           const std::set<std::string>& for_update) {
  auto sub = subscriptions_.find(type_url);
  RELEASE_ASSERT(sub != subscriptions_.end(),
                 fmt::format("Watch of {} has no subscription to update.", type_url));
  sub->second->sub_state_.updateSubscriptionInterest(for_update, {});
  // Tell the server about our change in interest, if any.
  if (sub->second->sub_state_.subscriptionUpdatePending()) {
    trySendDiscoveryRequests();
  }
}

void NewGrpcMuxImpl::removeWatch(const std::string& type_url, Watch* watch) {
  updateWatch(type_url, watch, {});
  auto entry = subscriptions_.find(type_url);
  ASSERT(entry != subscriptions_.end(),
         fmt::format("removeWatch() called for non-existent subscription {}.", type_url));
  entry->second->watch_map_.removeWatch(watch);
}

void NewGrpcMuxImpl::addSubscription(const std::string& type_url,
                                     const bool use_namespace_matching) {
  subscriptions_.emplace(type_url, std::make_unique<SubscriptionStuff>(
                                       type_url, local_info_, use_namespace_matching, dispatcher_));
  subscription_ordering_.emplace_back(type_url);
}

void NewGrpcMuxImpl::trySendDiscoveryRequests() {
  while (true) {
    // Do any of our subscriptions even want to send a request?
    absl::optional<std::string> maybe_request_type = whoWantsToSendDiscoveryRequest();
    if (!maybe_request_type.has_value()) {
      break;
    }
    // If so, which one (by type_url)?
    std::string next_request_type_url = maybe_request_type.value();
    // If we don't have a subscription object for this request's type_url, drop the request.
    auto sub = subscriptions_.find(next_request_type_url);
    RELEASE_ASSERT(sub != subscriptions_.end(),
                   fmt::format("Tried to send discovery request for non-existent subscription {}.",
                               next_request_type_url));

    // Try again later if paused/rate limited/stream down.
    if (!canSendDiscoveryRequest(next_request_type_url)) {
      break;
    }
    envoy::service::discovery::v3::DeltaDiscoveryRequest request;
    // Get our subscription state to generate the appropriate DeltaDiscoveryRequest, and send.
    if (!pausable_ack_queue_.empty()) {
      // Because ACKs take precedence over plain requests, if there is anything in the queue, it's
      // safe to assume it's of the type_url that we're wanting to send.
      request = sub->second->sub_state_.getNextRequestWithAck(pausable_ack_queue_.popFront());
    } else {
      request = sub->second->sub_state_.getNextRequestAckless();
    }
    VersionConverter::prepareMessageForGrpcWire(request, transport_api_version_);
    grpc_stream_.sendMessage(request);
  }
  grpc_stream_.maybeUpdateQueueSizeStat(pausable_ack_queue_.size());
}

// Checks whether external conditions allow sending a DeltaDiscoveryRequest. (Does not check
// whether we *want* to send a DeltaDiscoveryRequest).
bool NewGrpcMuxImpl::canSendDiscoveryRequest(const std::string& type_url) {
  RELEASE_ASSERT(
      !pausable_ack_queue_.paused(type_url),
      fmt::format("canSendDiscoveryRequest() called on paused type_url {}. Pausedness is "
                  "supposed to be filtered out by whoWantsToSendDiscoveryRequest(). ",
                  type_url));

  if (!grpc_stream_.grpcStreamAvailable()) {
    ENVOY_LOG(trace, "No stream available to send a discovery request for {}.", type_url);
    return false;
  } else if (!grpc_stream_.checkRateLimitAllowsDrain()) {
    ENVOY_LOG(trace, "{} discovery request hit rate limit; will try later.", type_url);
    return false;
  }
  return true;
}

// Checks whether we have something to say in a DeltaDiscoveryRequest, which can be an ACK and/or
// a subscription update. (Does not check whether we *can* send that DeltaDiscoveryRequest).
// Returns the type_url we should send the DeltaDiscoveryRequest for (if any).
// First, prioritizes ACKs over non-ACK subscription interest updates.
// Then, prioritizes non-ACK updates in the order the various types
// of subscriptions were activated.
absl::optional<std::string> NewGrpcMuxImpl::whoWantsToSendDiscoveryRequest() {
  // All ACKs are sent before plain updates. trySendDiscoveryRequests() relies on this. So, choose
  // type_url from pausable_ack_queue_ if possible, before looking at pending updates.
  if (!pausable_ack_queue_.empty()) {
    return pausable_ack_queue_.front().type_url_;
  }
  // If we're looking to send multiple non-ACK requests, send them in the order that their
  // subscriptions were initiated.
  for (const auto& sub_type : subscription_ordering_) {
    auto sub = subscriptions_.find(sub_type);
    if (sub != subscriptions_.end() && sub->second->sub_state_.subscriptionUpdatePending() &&
        !pausable_ack_queue_.paused(sub_type)) {
      return sub->first;
    }
  }
  return absl::nullopt;
}

} // namespace Config
} // namespace Envoy
