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

#ifndef GOOGLE_CLOUD_CPP_GOOGLE_CLOUD_BIGTABLE_INTERNAL_CLIENT_METRICS_OPTIONS_H
#define GOOGLE_CLOUD_CPP_GOOGLE_CLOUD_BIGTABLE_INTERNAL_CLIENT_METRICS_OPTIONS_H

#ifdef GOOGLE_CLOUD_CPP_BIGTABLE_WITH_OTEL_METRICS

#include "google/cloud/version.h"
#include <grpcpp/ext/otel_plugin.h>
#include <opentelemetry/metrics/meter_provider.h>
#include <memory>

namespace google {
namespace cloud {
namespace bigtable_internal {
GOOGLE_CLOUD_CPP_INLINE_NAMESPACE_BEGIN

/**
 * Internal option to store the configured gRPC OpenTelemetry plugin instance.
 *
 * This allows the plugin to be passed down to the stub factory where gRPC
 * channels are created.
 */
struct BigtableGrpcOtelPluginOption {
  using Type = std::shared_ptr<grpc::experimental::OpenTelemetryPlugin>;
};

/**
 * Internal option to store the MeterProvider used for Bigtable metrics.
 *
 * This allows the connection to flush metrics on shutdown.
 */
struct BigtableMeterProviderOption {
  using Type = std::shared_ptr<opentelemetry::metrics::MeterProvider>;
};

GOOGLE_CLOUD_CPP_INLINE_NAMESPACE_END
}  // namespace bigtable_internal
}  // namespace cloud
}  // namespace google

#endif  // GOOGLE_CLOUD_CPP_BIGTABLE_WITH_OTEL_METRICS

#endif  // GOOGLE_CLOUD_CPP_GOOGLE_CLOUD_BIGTABLE_INTERNAL_CLIENT_METRICS_OPTIONS_H
