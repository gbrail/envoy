#include <filesystem>

#include "common/buffer/zero_copy_input_stream_impl.h"
#include "common/grpc/common.h"

#include "extensions/filters/http/ext_proc/client_impl.h"

#include "test/mocks/grpc/mocks.h"
#include "test/mocks/stats/mocks.h"
#include "test/mocks/upstream/cluster_manager.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

using testing::_;
using testing::ByMove;
using testing::Invoke;
using testing::Return;
using testing::ReturnRef;
using testing::SaveArg;

using envoy::service::ext_proc::v3alpha::ProcessingRequest;
using envoy::service::ext_proc::v3alpha::ProcessingResponse;

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace ExternalProcessing {
namespace {

class MockCallbacks : public ExternalProcessorCallbacks {
public:
  MOCK_METHOD(void, onReceiveMessage, (std::unique_ptr<ProcessingResponse> && message));
  MOCK_METHOD(void, onGrpcError, (Grpc::Status::GrpcStatus error));
  MOCK_METHOD(void, onGrpcClose, ());
};

class ExternalProcessorClientTest : public testing::Test {
protected:
  void SetUp() {
    // Set up the environment to create a single mock gRPC bidirectional stream.
    scope_ = stats_store.createScope("testscope");
    EXPECT_CALL(cluster_manager, grpcAsyncClientManager())
        .WillOnce(ReturnRef(async_client_manager));
    EXPECT_CALL(async_client_manager, factoryForGrpcService(_, _, _))
        .WillOnce(Return(ByMove(std::unique_ptr<Grpc::AsyncClientFactory>(async_client_factory))));
    EXPECT_CALL(*async_client_factory, create())
        .WillOnce(Return(ByMove(std::unique_ptr<Grpc::RawAsyncClient>(async_client))));
    EXPECT_CALL(*async_client,
                startRaw("envoy.service.ext_proc.v3alpha.ExternalProcessor", "Process", _, _))
        .WillOnce(Invoke(this, &ExternalProcessorClientTest::handleStartRaw));
    EXPECT_CALL(*async_stream, sendMessageRaw_(_, _))
        .WillRepeatedly(Invoke(this, &ExternalProcessorClientTest::handleSendRaw));
    EXPECT_CALL(*async_stream, closeStream())
        .WillRepeatedly(Invoke(this, &ExternalProcessorClientTest::handleStreamClose));

    envoy::config::core::v3::GrpcService grpc_service;
    grpc_service.mutable_envoy_grpc()->set_cluster_name("testcluster");
    client_ = std::make_unique<ExternalProcessorClientImpl>(cluster_manager, grpc_service, *scope_);
  }

  void TearDown() {
    // TODO the fact that I have to do this worries me -- why isn't this
    // freed when the client is freed?
    delete async_stream;
  }

  Grpc::RawAsyncStream* handleStartRaw(absl::string_view, absl::string_view,
                                       Grpc::RawAsyncStreamCallbacks& cb,
                                       const Http::AsyncClient::StreamOptions&) {
    stream_callbacks_ = &cb;
    return async_stream;
  }

  // Parse a raw buffer that was "sent" to a gRPC stream back into a
  // protobuf that we can check against.
  void handleSendRaw(Buffer::InstancePtr& request, bool end_stream) {
    last_received_end_stream_ = end_stream;
    Buffer::ZeroCopyInputStreamImpl in_stream;
    in_stream.move(*request);
    in_stream.finish();
    last_received_request_.Clear();
    last_received_request_.ParseFromZeroCopyStream(&in_stream);
  }

  void handleStreamClose() { received_stream_close_ = true; }

  // All the mocks
  NiceMock<Stats::MockIsolatedStatsStore> stats_store;
  Stats::ScopePtr scope_;
  Upstream::MockClusterManager cluster_manager;
  Grpc::MockAsyncClientManager async_client_manager;
  Grpc::MockAsyncClientFactory* async_client_factory = new Grpc::MockAsyncClientFactory();
  Grpc::MockAsyncClient* async_client = new Grpc::MockAsyncClient();
  Grpc::MockAsyncStream* async_stream = new Grpc::MockAsyncStream();
  std::unique_ptr<ExternalProcessorClient> client_;

  // Callbacks for going back on the stream
  Grpc::RawAsyncStreamCallbacks* stream_callbacks_ = nullptr;

  // Stuff sent on the stream
  ProcessingRequest last_received_request_;
  bool last_received_end_stream_ = false;
  bool received_stream_close_ = false;
};

TEST_F(ExternalProcessorClientTest, SendRequestHeaders) {
  MockCallbacks callbacks;
  auto stream = client_->start(callbacks);

  ProcessingRequest request;
  request.set_response_required(true);
  auto hdrs_req = request.mutable_request_headers();
  auto hdr = hdrs_req->mutable_headers()->add_headers();
  hdr->set_key(":method");
  hdr->set_value("GET");
  hdr = hdrs_req->mutable_headers()->add_headers();
  hdr->set_key(":path");
  hdr->set_value("/hello");
  hdrs_req->set_end_of_stream(true);

  stream->send(std::move(request), false);

  EXPECT_FALSE(last_received_end_stream_);
  EXPECT_TRUE(last_received_request_.response_required());
  ASSERT_TRUE(last_received_request_.has_request_headers());
  EXPECT_TRUE(last_received_request_.request_headers().end_of_stream());
  ASSERT_TRUE(last_received_request_.request_headers().has_headers());
  EXPECT_EQ(2, last_received_request_.request_headers().headers().headers_size());
  EXPECT_FALSE(received_stream_close_);

  stream->close();

  EXPECT_TRUE(received_stream_close_);
}

TEST_F(ExternalProcessorClientTest, SendResponseBodyEOS) {
  MockCallbacks callbacks;
  auto stream = client_->start(callbacks);

  ProcessingRequest request;
  request.set_response_required(false);
  auto body_req = request.mutable_response_body();
  body_req->set_end_of_stream(true);
  body_req->set_body("Hello, World!");

  stream->send(std::move(request), true);

  EXPECT_TRUE(last_received_end_stream_);
  EXPECT_FALSE(last_received_request_.response_required());
  ASSERT_TRUE(last_received_request_.has_response_body());
  EXPECT_TRUE(last_received_request_.response_body().end_of_stream());
  EXPECT_EQ("Hello, World!", last_received_request_.response_body().body());
}

TEST_F(ExternalProcessorClientTest, ReceiveHeaderResponse) {
  MockCallbacks callbacks;
  auto stream = client_->start(callbacks);

  ASSERT_NE(nullptr, stream_callbacks_);

  ProcessingResponse response;
  auto hdrs = response.mutable_response_headers();
  auto mut = hdrs->mutable_header_mutation();
  mut->add_remove_headers("x-extraneous");

  std::unique_ptr<ProcessingResponse> received;
  EXPECT_CALL(callbacks, onReceiveMessage(_))
      .WillOnce(Invoke(
          [&received](std::unique_ptr<ProcessingResponse>&& p) { received = std::move(p); }));
  ;

  auto msg = Grpc::Common::serializeMessage(response);
  stream_callbacks_->onReceiveMessageRaw(std::move(msg));

  ASSERT_TRUE(received->has_response_headers());
  ASSERT_TRUE(received->response_headers().has_header_mutation());
  EXPECT_EQ(1, received->response_headers().header_mutation().remove_headers_size());
}

} // namespace
} // namespace ExternalProcessing
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy