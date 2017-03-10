#pragma once

#include "envoy/common/pure.h"
#include "envoy/redis/codec.h"

namespace Redis {
namespace CommandSplitter {

/**
 * A handle to request split.
 */
class ActiveRequest {
public:
  virtual ~ActiveRequest() {}

  /**
   * Cancel the request. No further request callbacks will be called.
   */
  virtual void cancel() PURE;
};

typedef std::unique_ptr<ActiveRequest> ActiveRequestPtr;

/**
 * Request split callbacks.
 */
class ActiveRequestCallbacks {
public:
  virtual ~ActiveRequestCallbacks() {}

  /**
   * Called when the response is ready.
   * @param value supplies the response which is now owned by the callee.
   */
  virtual void onResponse(RespValuePtr&& value) PURE;
};

/**
 * fixfix
 */
class Instance {
public:
  virtual ~Instance() {}

  /**
   * fixfix
   * @param request supplies the request to make.
   * @param callbacks supplies the request completion callbacks.
   * @return ActiveRequestPtr a handle to the active request or fixfix
   */
  virtual ActiveRequestPtr makeRequest(const RespValue& request,
                                       ActiveRequestCallbacks& callbacks) PURE;
};

} // CommandSplitter
} // Redis
