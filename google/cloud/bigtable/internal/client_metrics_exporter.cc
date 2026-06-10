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

#include "google/cloud/bigtable/internal/client_metrics_exporter.h"
#include "google/cloud/monitoring/v3/metric_connection.h"
#include "google/cloud/opentelemetry/internal/monitoring_exporter.h"
#include "google/cloud/opentelemetry/monitoring_exporter.h"
#include "google/cloud/grpc_options.h"
#include "google/cloud/universe_domain_options.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_replace.h"

namespace google {
namespace cloud {
namespace bigtable_internal {
GOOGLE_CLOUD_CPP_INLINE_NAMESPACE_BEGIN

std::unique_ptr<opentelemetry::sdk::metrics::PushMetricExporter>
CreateClientMetricsExporter(
    Project project, google::api::MonitoredResource const& monitored_resource,
    Options const& options) {
  // Construct options for the Monitoring service connection.
  // We start with a copy of the client options to preserve credentials and
  // universe domain, but we must unset Bigtable-specific endpoints/authorities
  // to allow default Monitoring defaults.
  auto connection_options = options;
  connection_options.unset<EndpointOption>();
  connection_options.unset<AuthorityOption>();

  auto conn =
      monitoring_v3::MakeMetricServiceConnection(std::move(connection_options));

  // Configure exporter options.
  auto exporter_options =
      Options{}
          .set<otel::ServiceTimeSeriesOption>(true)
          .set<otel::MonitoredResourceOption>(monitored_resource)
          .set<otel::MetricNameFormatterOption>([](std::string const& name) {
            std::string formatted_name = name;
            if (formatted_name ==
                "grpc.client.attempt.rcvd_total_compressed_message_size") {
              formatted_name =
                  "grpc.client.attempt.received_total_compressed_message_size";
            }
            return absl::StrCat("bigtable.googleapis.com/internal/client/",
                                absl::StrReplaceAll(formatted_name, {{".", "/"}}));
          })
          .set<otel_internal::ResourceFilterDataFnOption>({"service_name"});

  return otel::MakeMonitoringExporter(std::move(project), std::move(conn),
                                      std::move(exporter_options));
}

GOOGLE_CLOUD_CPP_INLINE_NAMESPACE_END
}  // namespace bigtable_internal
}  // namespace cloud
}  // namespace google

#endif  // GOOGLE_CLOUD_CPP_BIGTABLE_WITH_OTEL_METRICS
