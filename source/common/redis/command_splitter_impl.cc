#include "command_splitter_impl.h"

#include "common/common/assert.h"

namespace Redis {
namespace CommandSplitter {

AllParamsToOneServerCommandHandler AllParamsToOneServerCommandHandler::instance_;
OneParamToOneServerCommandHandler OneParamToOneServerCommandHandler::instance_;

InstanceImpl::InstanceImpl(ConnPool::InstancePtr&& conn_pool) : conn_pool_(std::move(conn_pool)) {
  // TODO(mattklein123) PERF: Make this a trie (like in header_map_impl).
  command_map_.emplace("incr", AllParamsToOneServerCommandHandler::instance_);
  command_map_.emplace("incrby", AllParamsToOneServerCommandHandler::instance_);
  command_map_.emplace("mget", OneParamToOneServerCommandHandler::instance_);
}

ActiveRequestPtr InstanceImpl::makeRequest(const RespValue& request, ActiveRequestCallbacks&) {
  if (request.type() != RespType::Array ||
      request.asArray().empty() ||
      request.asArray()[0].type() != RespType::BulkString) {
    ASSERT(false); //fixfix
  }

  auto handler = command_map_.find(request.asArray()[0].asString());
  if (handler == command_map_.end()) {
    ASSERT(false); //fixfix
  }

  return handler->second.get().startRequest(request);
}

} // CommandSplitter
} // Redis
