// Copyright 2026 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifdef GOOGLE_CLOUD_CPP_BIGTABLE_WITH_OTEL_METRICS

#include "google/cloud/bigtable/internal/client_metrics_resource.h"
#include "google/cloud/bigtable/options.h"
#include "google/cloud/internal/detect_gcp.h"
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <opentelemetry/sdk/resource/resource.h>
#include <opentelemetry/sdk/resource/resource_detector.h>

namespace google {
namespace cloud {
namespace bigtable_internal {
GOOGLE_CLOUD_CPP_INLINE_NAMESPACE_BEGIN
namespace {

using ::testing::Return;

class MockGcpDetector : public google::cloud::internal::GcpDetector {
 public:
  MOCK_METHOD(bool, IsGoogleCloudBios, (), (override));
  MOCK_METHOD(bool, IsGoogleCloudServerless, (), (override));
};

class MockResourceDetector
    : public opentelemetry::sdk::resource::ResourceDetector {
 public:
  MOCK_METHOD(opentelemetry::sdk::resource::Resource, Detect, (), (override));
};

Options TestOptions() {
  return Options{}
      .set<google::cloud::bigtable::ProjectIdOption>("test-project")
      .set<google::cloud::bigtable::InstanceIdOption>("test-instance")
      .set<google::cloud::bigtable::AppProfileIdOption>("test-profile");
}

TEST(ClientMetricsResourceTest, NonGcpEnvironment) {
  auto mock_gcp = std::make_shared<MockGcpDetector>();
  EXPECT_CALL(*mock_gcp, IsGoogleCloudBios()).WillRepeatedly(Return(false));
  EXPECT_CALL(*mock_gcp, IsGoogleCloudServerless())
      .WillRepeatedly(Return(false));

  auto mr = BuildBigtableClientResource(TestOptions(), mock_gcp, nullptr);

  EXPECT_EQ(mr.type(), "bigtable_client");
  auto const& labels = mr.labels();
  EXPECT_EQ(labels.at("project_id"), "test-project");
  EXPECT_EQ(labels.at("instance"), "test-instance");
  EXPECT_EQ(labels.at("app_profile"), "test-profile");
  EXPECT_TRUE(labels.at("client_name").find("cpp.Bigtable/") == 0);
  EXPECT_FALSE(labels.at("uuid").empty());
  EXPECT_EQ(labels.at("client_project"), "unknown");
  EXPECT_EQ(labels.at("cloud_platform"), "unknown");
  EXPECT_EQ(labels.at("region"), "global");
  EXPECT_EQ(labels.at("host_id"), "unknown");
  EXPECT_EQ(labels.at("host_name"), "unknown");
}

TEST(ClientMetricsResourceTest, GcpEnvironmentGceGke) {
  auto mock_gcp = std::make_shared<MockGcpDetector>();
  EXPECT_CALL(*mock_gcp, IsGoogleCloudBios()).WillRepeatedly(Return(true));
  EXPECT_CALL(*mock_gcp, IsGoogleCloudServerless())
      .WillRepeatedly(Return(false));

  auto mock_otel = std::make_unique<MockResourceDetector>();
  EXPECT_CALL(*mock_otel, Detect()).WillOnce([]() {
    return opentelemetry::sdk::resource::Resource::Create({
        {"cloud.account.id", "gcp-project"},
        {"cloud.platform", "gcp_compute_engine"},
        {"cloud.region", "us-central1"},
        {"host.id", "gce-host-id"},
        {"host.name", "gce-host-name"},
    });
  });

  auto mr = BuildBigtableClientResource(TestOptions(), mock_gcp,
                                        std::move(mock_otel));

  EXPECT_EQ(mr.type(), "bigtable_client");
  auto const& labels = mr.labels();
  EXPECT_EQ(labels.at("project_id"), "test-project");
  EXPECT_EQ(labels.at("instance"), "test-instance");
  EXPECT_EQ(labels.at("app_profile"), "test-profile");
  EXPECT_TRUE(labels.at("client_name").find("cpp.Bigtable/") == 0);
  EXPECT_FALSE(labels.at("uuid").empty());
  EXPECT_EQ(labels.at("client_project"), "gcp-project");
  EXPECT_EQ(labels.at("cloud_platform"), "gcp_compute_engine");
  EXPECT_EQ(labels.at("region"), "us-central1");
  EXPECT_EQ(labels.at("host_id"), "gce-host-id");
  EXPECT_EQ(labels.at("host_name"), "gce-host-name");
}

TEST(ClientMetricsResourceTest, GcpEnvironmentFallbackRegion) {
  auto mock_gcp = std::make_shared<MockGcpDetector>();
  EXPECT_CALL(*mock_gcp, IsGoogleCloudBios()).WillRepeatedly(Return(true));
  EXPECT_CALL(*mock_gcp, IsGoogleCloudServerless())
      .WillRepeatedly(Return(false));

  auto mock_otel = std::make_unique<MockResourceDetector>();
  EXPECT_CALL(*mock_otel, Detect()).WillOnce([]() {
    return opentelemetry::sdk::resource::Resource::Create({
        {"cloud.account.id", "gcp-project"},
        {"cloud.platform", "gcp_compute_engine"},
        {"cloud.availability_zone", "us-east1-b"},
        {"host.id", "gce-host-id"},
        {"host.name", "gce-host-name"},
    });
  });

  auto mr = BuildBigtableClientResource(TestOptions(), mock_gcp,
                                        std::move(mock_otel));

  EXPECT_EQ(mr.type(), "bigtable_client");
  auto const& labels = mr.labels();
  EXPECT_EQ(labels.at("region"), "us-east1");
}

TEST(ClientMetricsResourceTest, GcpEnvironmentOtelDetectionFails) {
  auto mock_gcp = std::make_shared<MockGcpDetector>();
  EXPECT_CALL(*mock_gcp, IsGoogleCloudBios()).WillRepeatedly(Return(true));
  EXPECT_CALL(*mock_gcp, IsGoogleCloudServerless())
      .WillRepeatedly(Return(false));

  auto mock_otel = std::make_unique<MockResourceDetector>();
  EXPECT_CALL(*mock_otel, Detect()).WillOnce([]() {
    return opentelemetry::sdk::resource::Resource::Create({});
  });

  auto mr = BuildBigtableClientResource(TestOptions(), mock_gcp,
                                        std::move(mock_otel));

  EXPECT_EQ(mr.type(), "bigtable_client");
  auto const& labels = mr.labels();
  EXPECT_EQ(labels.at("client_project"), "unknown");
  EXPECT_EQ(labels.at("cloud_platform"), "unknown");
  EXPECT_EQ(labels.at("region"), "global");
  EXPECT_EQ(labels.at("host_id"), "unknown");
  EXPECT_EQ(labels.at("host_name"), "unknown");
}

TEST(ClientMetricsResourceTest, PiiProtection) {
  auto mock_gcp = std::make_shared<MockGcpDetector>();
  EXPECT_CALL(*mock_gcp, IsGoogleCloudBios()).WillRepeatedly(Return(true));
  EXPECT_CALL(*mock_gcp, IsGoogleCloudServerless())
      .WillRepeatedly(Return(false));

  auto mock_otel = std::make_unique<MockResourceDetector>();
  EXPECT_CALL(*mock_otel, Detect()).WillOnce([]() {
    return opentelemetry::sdk::resource::Resource::Create({
        {"cloud.platform", "unknown"},
        {"host.id", "pii-host-id"},
        {"host.name", "pii-host-name"},
    });
  });

  auto mr = BuildBigtableClientResource(TestOptions(), mock_gcp,
                                        std::move(mock_otel));

  EXPECT_EQ(mr.type(), "bigtable_client");
  auto const& labels = mr.labels();
  EXPECT_EQ(labels.at("cloud_platform"), "unknown");
  EXPECT_EQ(labels.at("host_id"), "unknown");
  EXPECT_EQ(labels.at("host_name"), "unknown");
}

TEST(ClientMetricsResourceTest, GcpEnvironmentFallbackRegionGovCloud) {
  auto mock_gcp = std::make_shared<MockGcpDetector>();
  EXPECT_CALL(*mock_gcp, IsGoogleCloudBios()).WillRepeatedly(Return(true));
  EXPECT_CALL(*mock_gcp, IsGoogleCloudServerless())
      .WillRepeatedly(Return(false));

  auto mock_otel = std::make_unique<MockResourceDetector>();
  EXPECT_CALL(*mock_otel, Detect()).WillOnce([]() {
    return opentelemetry::sdk::resource::Resource::Create({
        {"cloud.account.id", "gcp-project"},
        {"cloud.platform", "gcp_compute_engine"},
        {"cloud.availability_zone", "us-gov-west1-a"},
        {"host.id", "gce-host-id"},
        {"host.name", "gce-host-name"},
    });
  });

  auto mr = BuildBigtableClientResource(TestOptions(), mock_gcp,
                                        std::move(mock_otel));

  EXPECT_EQ(mr.type(), "bigtable_client");
  auto const& labels = mr.labels();
  EXPECT_EQ(labels.at("region"), "us-gov-west1");
}

TEST(ClientMetricsResourceTest, GrpcEnableMetricsIsSafeTest) {
  // Unsafe versions (<= 1.62.x)
  EXPECT_FALSE(GrpcEnableMetricsIsSafe(0, 9, 0));
  EXPECT_FALSE(GrpcEnableMetricsIsSafe(1, 62, 0));
  EXPECT_FALSE(GrpcEnableMetricsIsSafe(1, 62, 9));

  // Unsafe version 1.63.0
  EXPECT_FALSE(GrpcEnableMetricsIsSafe(1, 63, 0));

  // Safe versions 1.63.1+
  EXPECT_TRUE(GrpcEnableMetricsIsSafe(1, 63, 1));
  EXPECT_TRUE(GrpcEnableMetricsIsSafe(1, 63, 2));

  // Unsafe version 1.64.0
  EXPECT_FALSE(GrpcEnableMetricsIsSafe(1, 64, 0));

  // Safe versions 1.64.1+
  EXPECT_TRUE(GrpcEnableMetricsIsSafe(1, 64, 1));
  EXPECT_TRUE(GrpcEnableMetricsIsSafe(1, 64, 2));

  // Safe versions >= 1.65.x
  EXPECT_TRUE(GrpcEnableMetricsIsSafe(1, 65, 0));
  EXPECT_TRUE(GrpcEnableMetricsIsSafe(1, 66, 0));
  EXPECT_TRUE(GrpcEnableMetricsIsSafe(2, 0, 0));
}

}  // namespace
GOOGLE_CLOUD_CPP_INLINE_NAMESPACE_END
}  // namespace bigtable_internal
}  // namespace cloud
}  // namespace google

#endif  // GOOGLE_CLOUD_CPP_BIGTABLE_WITH_OTEL_METRICS
