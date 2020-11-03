#include "extensions/filters/http/ext_proc/ext_proc.h"

#include <string_view>

#include "envoy/config/core/v3/base.pb.h"

#include "test/extensions/filters/http/ext_proc/client_mocks.h"
#include "test/mocks/http/mocks.h"
#include "test/mocks/network/mocks.h"
#include "test/mocks/router/mocks.h"
#include "test/mocks/runtime/mocks.h"
#include "test/mocks/tracing/mocks.h"
#include "test/mocks/upstream/cluster_manager.h"
#include "test/test_common/printers.h"
#include "test/test_common/utility.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

using envoy::config::core::v3::HeaderMap;
using envoy::service::ext_proc::v3alpha::ProcessingRequest;
using envoy::service::ext_proc::v3alpha::ProcessingResponse;

using testing::Assign;
using testing::Invoke;

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace ExternalProcessing {
namespace {

class ExtProcFilterTest : public ::testing::Test {
protected:
  void SetUp() override {
    auto client = new MockClient();
    EXPECT_CALL(*client, start(_)).WillOnce(Invoke(this, &ExtProcFilterTest::startStream));
    client_ = std::unique_ptr<ExternalProcessorClient>(client);

    auto stream = new MockStream();
    EXPECT_CALL(*stream, close()).WillOnce(Invoke(this, &ExtProcFilterTest::streamClose));
    EXPECT_CALL(*stream, send(_, _)).WillRepeatedly(Invoke(this, &ExtProcFilterTest::streamSend));
    stream_ = stream;
  }

  void initialize(std::string&& yaml) {
    envoy::extensions::filters::http::ext_proc::v3alpha::ExternalProcessor proto_config{};
    if (!yaml.empty()) {
      TestUtility::loadFromYaml(yaml, proto_config);
    }
    config_.reset(new FilterConfig(proto_config));
    filter_ = std::make_unique<Filter>(config_, std::move(client_));
    filter_->setEncoderFilterCallbacks(encoder_callbacks_);
    EXPECT_CALL(encoder_callbacks_, continueEncoding())
        .WillRepeatedly(Assign(&encoding_continued_, true));
    filter_->setDecoderFilterCallbacks(decoder_callbacks_);
    EXPECT_CALL(decoder_callbacks_, continueDecoding())
        .WillRepeatedly(Assign(&decoding_continued_, true));
  }

  ExternalProcessorStreamPtr startStream(ExternalProcessorCallbacks& callbacks) {
    callbacks_ = &callbacks;
    return std::unique_ptr<ExternalProcessorStream>(stream_);
  }

  void streamClose() { client_closed_ = true; }

  void streamSend(ProcessingRequest&& request, bool end_stream) {
    last_request_ = std::move(request);
    sent_end_stream_ = end_stream;
  }

  void checkHeader(const HeaderMap& map, std::string_view key, std::string_view value) {
    for (auto val = map.headers().cbegin(); val != map.headers().cend(); val++) {
      if (val->key() == key) {
        EXPECT_EQ(value, val->value());
        return;
      }
    }
    EXPECT_FALSE(true);
  }

  void checkEnvoyHeader(const Http::HeaderMap& map, std::string_view key, std::string_view value) {
    auto hdr = map.get(Http::LowerCaseString(std::string(key)));
    EXPECT_FALSE(hdr.empty());
    if (hdr.empty()) {
      return;
    }
    EXPECT_EQ(1, hdr.size());
    EXPECT_EQ(value, hdr[0]->value().getStringView());
  }

  void checkEnvoyHeaderNotPresent(const Http::HeaderMap& map, std::string_view key) {
    auto hdr = map.get(Http::LowerCaseString(std::string(key)));
    EXPECT_TRUE(hdr.empty());
  }

  // Stuff we change during setup
  std::unique_ptr<ExternalProcessorClient> client_;
  ExternalProcessorStream* stream_;

  // Stuff that stays static
  NiceMock<Stats::MockIsolatedStatsStore> stats_store_;
  FilterConfigSharedPtr config_;
  std::unique_ptr<Filter> filter_;
  Http::MockStreamDecoderFilterCallbacks decoder_callbacks_;
  Http::MockStreamEncoderFilterCallbacks encoder_callbacks_;
  Http::TestRequestHeaderMapImpl request_headers_;
  Http::TestResponseHeaderMapImpl response_headers_;
  Http::TestRequestTrailerMapImpl request_trailers_;
  Http::TestResponseTrailerMapImpl response_trailers_;
  Buffer::OwnedImpl data_;

  // Stuff that changes every time
  ExternalProcessorCallbacks* callbacks_;
  ProcessingRequest last_request_;
  bool sent_end_stream_ = false;
  bool client_closed_ = false;
  bool decoding_continued_ = false;
  bool encoding_continued_ = false;
};

TEST_F(ExtProcFilterTest, SimplestPost) {
  initialize(R"EOF(
  grpc_service:
    envoy_grpc:
      cluster_name: "ext_authz_server"
  )EOF");

  EXPECT_FALSE(config_->failureModeAllow());

  // Send a simple post
  request_headers_.setMethod("POST");
  request_headers_.setPath("/cats");
  request_headers_.setContentType("application/json");
  request_headers_.setContentLength(12);
  EXPECT_EQ(Http::FilterHeadersStatus::StopAllIterationAndWatermark,
            filter_->decodeHeaders(request_headers_, false));
  EXPECT_FALSE(decoding_continued_);

  // Remote server should have received it
  ASSERT_TRUE(last_request_.has_request_headers());
  checkHeader(last_request_.request_headers().headers(), ":method", "POST");
  checkHeader(last_request_.request_headers().headers(), ":path", "/cats");
  checkHeader(last_request_.request_headers().headers(), "content-type", "application/json");
  checkHeader(last_request_.request_headers().headers(), "content-length", "12");
  EXPECT_FALSE(last_request_.request_headers().end_of_stream());

  // Remote server wants to add a header, and delete one
  auto req_hdr_response = std::make_unique<ProcessingResponse>();
  auto new_hdr =
      req_hdr_response->mutable_request_headers()->mutable_header_mutation()->add_set_headers();
  new_hdr->mutable_header()->set_key("X-I-Was-Here");
  new_hdr->mutable_header()->set_value("Yes!");
  req_hdr_response->mutable_request_headers()->mutable_header_mutation()->add_remove_headers(
      "content-length");
  callbacks_->onReceiveMessage(std::move(req_hdr_response));

  // It should have been added, and we should have continued
  checkEnvoyHeader(request_headers_, "x-i-was-here", "Yes!");
  checkEnvoyHeaderNotPresent(request_headers_, "content-length");
  EXPECT_TRUE(decoding_continued_);

  data_.add("foo");
  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_->decodeData(data_, true));
  EXPECT_EQ(Http::FilterTrailersStatus::Continue, filter_->decodeTrailers(request_trailers_));

  EXPECT_EQ(Http::FilterHeadersStatus::Continue,
            filter_->encode100ContinueHeaders(response_headers_));
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->encodeHeaders(response_headers_, true));
  data_.add("bar");
  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_->encodeData(data_, false));
  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_->encodeData(data_, true));
  EXPECT_EQ(Http::FilterTrailersStatus::Continue, filter_->encodeTrailers(response_trailers_));
  filter_->onDestroy();
}

} // namespace
} // namespace ExternalProcessing
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy