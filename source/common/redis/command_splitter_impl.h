#pragma once

#include "envoy/redis/command_splitter.h"
#include "envoy/redis/conn_pool.h"

namespace Redis {
namespace CommandSplitter {

struct ActiveRequestImpl : public ActiveRequest {

  RespValuePtr pending_response_;
  ConnPool::ActiveRequest* request_handle_;
};

class CommandHandler {
public:
  virtual ~CommandHandler(){};

  virtual ActiveRequestPtr startRequest(const RespValue& request) PURE;
};

class AllParamsToOneServerCommandHandler : public CommandHandler {
public:
  // Redis::CommandSplitter::CommandHandler
  ActiveRequestPtr startRequest(const RespValue&) override;

  static AllParamsToOneServerCommandHandler instance_;
};

class OneParamToOneServerCommandHandler : public CommandHandler {
public:
  // Redis::CommandSplitter::CommandHandler
  ActiveRequestPtr startRequest(const RespValue&) override;

  static OneParamToOneServerCommandHandler instance_;
};

class InstanceImpl : public Instance {
public:
  InstanceImpl(ConnPool::InstancePtr&& conn_pool);

  // Redis::CommandSplitter::Instance
  ActiveRequestPtr makeRequest(const RespValue& request,
                               ActiveRequestCallbacks& callbacks) override;

private:
  ConnPool::InstancePtr conn_pool_;
  std::unordered_map<std::string, std::reference_wrapper<CommandHandler>> command_map_;
};

} // CommandSplitter
} // Redis
