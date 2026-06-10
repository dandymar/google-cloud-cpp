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

#ifndef GOOGLE_CLOUD_CPP_GOOGLE_CLOUD_BIGTABLE_INTERNAL_CLIENT_METRICS_RESOURCE_H
#define GOOGLE_CLOUD_CPP_GOOGLE_CLOUD_BIGTABLE_INTERNAL_CLIENT_METRICS_RESOURCE_H

#ifdef GOOGLE_CLOUD_CPP_BIGTABLE_WITH_OTEL_METRICS

#include "google/cloud/internal/detect_gcp.h"
#include "google/cloud/options.h"
#include "google/cloud/version.h"
#include <google/api/monitored_resource.pb.h>
#include <opentelemetry/sdk/resource/resource_detector.h>
#include <memory>

namespace google {
namespace cloud {
namespace bigtable_internal {
GOOGLE_CLOUD_CPP_INLINE_NAMESPACE_BEGIN

/**
 * Checks if the compiled gRPC runtime version is safe for OpenTelemetry
 * metrics.
 *
 * gRPC versions <= 1.64.0 (with some exceptions) have a memory corruption bug
 * when OTel metrics are registered, which can lead to application crashes.
 */
bool GrpcEnableMetricsIsSafe(int major, int minor, int patch);
bool GrpcEnableMetricsIsSafe();

/**
 * Builds the static MonitoredResource representation of a "bigtable_client"
 * with all required 10 attributes mapped from the environment detector.
 */
google::api::MonitoredResource BuildBigtableClientResource(
    Options const& options,
    std::shared_ptr<internal::GcpDetector> const& gcp_detector =
        internal::MakeGcpDetector(),
    std::unique_ptr<opentelemetry::sdk::resource::ResourceDetector>
        resource_detector = nullptr);

GOOGLE_CLOUD_CPP_INLINE_NAMESPACE_END
}  // namespace bigtable_internal
}  // namespace cloud
}  // namespace google

#endif  // GOOGLE_CLOUD_CPP_BIGTABLE_WITH_OTEL_METRICS

#endif  // GOOGLE_CLOUD_CPP_GOOGLE_CLOUD_BIGTABLE_INTERNAL_CLIENT_METRICS_RESOURCE_H
