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

#ifndef GOOGLE_CLOUD_CPP_GOOGLE_CLOUD_BIGTABLE_INTERNAL_CLIENT_METRICS_METER_PROVIDER_H
#define GOOGLE_CLOUD_CPP_GOOGLE_CLOUD_BIGTABLE_INTERNAL_CLIENT_METRICS_METER_PROVIDER_H

#ifdef GOOGLE_CLOUD_CPP_BIGTABLE_WITH_OTEL_METRICS

#include "google/cloud/options.h"
#include "google/cloud/version.h"
#include <grpcpp/ext/otel_plugin.h>
#include <opentelemetry/metrics/meter_provider.h>
#include <opentelemetry/sdk/metrics/push_metric_exporter.h>
#include <memory>

namespace google {
namespace cloud {
namespace bigtable_internal {
GOOGLE_CLOUD_CPP_INLINE_NAMESPACE_BEGIN

/**
 * Creates the MeterProvider configured with custom latency boundaries for
 * Bigtable client metrics.
 */
std::shared_ptr<opentelemetry::metrics::MeterProvider>
MakeClientMetricsMeterProvider(
    std::unique_ptr<opentelemetry::sdk::metrics::PushMetricExporter> exporter,
    Options const& options);

/**
 * Creates the local gRPC OpenTelemetry plugin configured for Bigtable client
 * metrics.
 */
absl::StatusOr<std::shared_ptr<grpc::experimental::OpenTelemetryPlugin>>
CreateBigtableGrpcOtelPlugin(
    std::shared_ptr<opentelemetry::metrics::MeterProvider> const& provider,
    Options const& options);

GOOGLE_CLOUD_CPP_INLINE_NAMESPACE_END
}  // namespace bigtable_internal
}  // namespace cloud
}  // namespace google

#endif  // GOOGLE_CLOUD_CPP_BIGTABLE_WITH_OTEL_METRICS

#endif  // GOOGLE_CLOUD_CPP_GOOGLE_CLOUD_BIGTABLE_INTERNAL_CLIENT_METRICS_METER_PROVIDER_H
