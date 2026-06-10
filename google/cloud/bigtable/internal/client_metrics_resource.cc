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
#include "google/cloud/opentelemetry/resource_detector.h"
#include "google/cloud/common_options.h"
#include "google/cloud/internal/getenv.h"
#include "google/cloud/internal/random.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "absl/strings/str_replace.h"
#include <grpcpp/grpcpp.h>
#include <opentelemetry/nostd/variant.h>
#include <opentelemetry/sdk/resource/resource.h>
#include <opentelemetry/semconv/incubating/cloud_attributes.h>
#include <opentelemetry/semconv/incubating/host_attributes.h>
#include <opentelemetry/semconv/incubating/k8s_attributes.h>
#include <opentelemetry/semconv/incubating/service_attributes.h>
#include <algorithm>
#include <cctype>

namespace google {
namespace cloud {
namespace bigtable_internal {
GOOGLE_CLOUD_CPP_INLINE_NAMESPACE_BEGIN
namespace {

namespace sc = opentelemetry::semconv;

struct AsStringVisitor {
  template <typename T>
  std::string operator()(std::vector<T> const& v) const {
    return absl::StrCat("[",
                        absl::StrJoin(v, ", ",
                                      [this](std::string* out, T const& v) {
                                        out->append(this->operator()(v));
                                      }),
                        "]");
  }
  template <typename T>
  std::string operator()(T const& v) const {
    return absl::StrCat(v);
  }
  std::string operator()(bool const& v) const { return v ? "true" : "false"; }
};

std::string AsString(
    opentelemetry::sdk::common::OwnedAttributeValue const& value) {
  return opentelemetry::nostd::visit(AsStringVisitor{}, value);
}

std::string ExtractRegion(std::string const& zone) {
  if (zone.length() >= 3 && zone[zone.length() - 2] == '-' &&
      std::islower(static_cast<unsigned char>(zone.back()))) {
    return zone.substr(0, zone.length() - 2);
  }
  return zone;
}

std::string GenerateUuid() {
  auto gen = internal::MakeDefaultPRNG();
  return absl::StrCat(
      "cpp-",
      internal::Sample(gen, 16, "abcdefghijklmnopqrstuvwxyz0123456789"));
}

}  // namespace

bool GrpcEnableMetricsIsSafe(int major, int minor, int patch) {
  if (major < 1) return false;
  if (major > 1) return true;
  if (minor <= 62) return false;
  if (minor == 63) return patch >= 1;
  if (minor == 64) return patch >= 1;
  return true;
}

bool GrpcEnableMetricsIsSafe() {
#ifndef GRPC_CPP_VERSION_MAJOR
  return false;
#else
  return GrpcEnableMetricsIsSafe(GRPC_CPP_VERSION_MAJOR, GRPC_CPP_VERSION_MINOR,
                                 GRPC_CPP_VERSION_PATCH);
#endif
}

google::api::MonitoredResource BuildBigtableClientResource(
    Options const& options,
    std::shared_ptr<internal::GcpDetector> const& gcp_detector,
    std::unique_ptr<opentelemetry::sdk::resource::ResourceDetector>
        resource_detector) {
  google::api::MonitoredResource mr;
  mr.set_type("bigtable_client");
  auto* labels = mr.mutable_labels();

  // 1. Static project and instance attributes
  std::string project_id =
      options.get<google::cloud::bigtable::ProjectIdOption>();
  std::string instance_id =
      options.get<google::cloud::bigtable::InstanceIdOption>();
  std::string app_profile =
      options.get<google::cloud::bigtable::AppProfileIdOption>();

  (*labels)["project_id"] = project_id;
  (*labels)["instance"] = instance_id;
  (*labels)["app_profile"] = app_profile.empty() ? "default" : app_profile;
  (*labels)["client_name"] =
      absl::StrCat("cpp.Bigtable/", google::cloud::bigtable::version_string());
  (*labels)["uuid"] = GenerateUuid();

  // 2. Default fallback values
  std::string client_project = "unknown";
  std::string cloud_platform = "unknown";
  std::string region = "global";
  std::string host_id = "unknown";
  std::string host_name = "unknown";

  // 3. Dynamic Environment Detection (only execute if we are on a GCP BIOS or
  // Serverless platform)
  if (gcp_detector->IsGoogleCloudBios() ||
      gcp_detector->IsGoogleCloudServerless()) {
    auto detector = std::move(resource_detector);
    if (!detector) {
      detector = otel::MakeResourceDetector();
    }
    if (detector) {
      auto resource = detector->Detect();
      auto const& attributes = resource.GetAttributes();

      auto it = attributes.find(sc::cloud::kCloudAccountId);
      if (it != attributes.end()) {
        client_project = AsString(it->second);
      }
      it = attributes.find(sc::cloud::kCloudPlatform);
      if (it != attributes.end()) {
        cloud_platform = AsString(it->second);
      }
      it = attributes.find(sc::cloud::kCloudRegion);
      if (it != attributes.end()) {
        region = AsString(it->second);
      } else {
        it = attributes.find(sc::cloud::kCloudAvailabilityZone);
        if (it != attributes.end()) {
          region = ExtractRegion(AsString(it->second));
        }
      }
      it = attributes.find(sc::host::kHostId);
      if (it != attributes.end()) {
        host_id = AsString(it->second);
      } else {
        it = attributes.find("faas.id");
        if (it != attributes.end()) {
          host_id = AsString(it->second);
        }
      }
      it = attributes.find(sc::host::kHostName);
      if (it != attributes.end()) {
        host_name = AsString(it->second);
      } else {
        it = attributes.find(sc::k8s::kK8sPodName);
        if (it != attributes.end()) {
          host_name = AsString(it->second);
        } else {
          it = attributes.find(sc::k8s::kK8sNodeName);
          if (it != attributes.end()) {
            host_name = AsString(it->second);
          }
        }
      }
    }
  }

  // 4. PII host details leakage protection outside GCP
  if (cloud_platform == "unknown") {
    host_id = "unknown";
    host_name = "unknown";
  }

  (*labels)["client_project"] = client_project;
  (*labels)["cloud_platform"] = cloud_platform;
  (*labels)["region"] = region;
  (*labels)["host_id"] = host_id;
  (*labels)["host_name"] = host_name;

  return mr;
}

GOOGLE_CLOUD_CPP_INLINE_NAMESPACE_END
}  // namespace bigtable_internal
}  // namespace cloud
}  // namespace google

#endif  // GOOGLE_CLOUD_CPP_BIGTABLE_WITH_OTEL_METRICS
