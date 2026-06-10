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

#include "google/cloud/bigtable/internal/client_metrics_meter_provider.h"
#include "google/cloud/bigtable/options.h"
#include "absl/strings/match.h"
#include <grpcpp/grpcpp.h>
#include <opentelemetry/sdk/metrics/export/periodic_exporting_metric_reader.h>
#if OPENTELEMETRY_VERSION_MAJOR > 1 || \
    (OPENTELEMETRY_VERSION_MAJOR == 1 && OPENTELEMETRY_VERSION_MINOR >= 10)
#include <opentelemetry/sdk/metrics/export/periodic_exporting_metric_reader_factory.h>
#include <opentelemetry/sdk/metrics/meter_provider_factory.h>
#include <opentelemetry/sdk/metrics/view/instrument_selector_factory.h>
#include <opentelemetry/sdk/metrics/view/meter_selector_factory.h>
#include <opentelemetry/sdk/metrics/view/view_factory.h>
#endif
#include <opentelemetry/sdk/metrics/meter_provider.h>
#include <opentelemetry/sdk/metrics/view/instrument_selector.h>
#include <opentelemetry/sdk/metrics/view/meter_selector.h>
#include <opentelemetry/sdk/metrics/view/view.h>
#include <opentelemetry/sdk/resource/resource.h>
#include <vector>

namespace google {
namespace cloud {
namespace bigtable_internal {
GOOGLE_CLOUD_CPP_INLINE_NAMESPACE_BEGIN
namespace {

std::vector<double> MakeLatencyHistogramBoundaries() {
  return {0.0,   0.001, 0.002, 0.003, 0.004, 0.005, 0.006,  0.008,
          0.01,  0.013, 0.016, 0.02,  0.025, 0.03,  0.04,   0.05,
          0.065, 0.08,  0.1,   0.13,  0.16,  0.2,   0.25,   0.3,
          0.4,   0.5,   0.65,  0.8,   1.0,   2.0,   5.0,    10.0,
          20.0,  50.0,  100.0, 200.0, 400.0, 800.0, 1600.0, 3200.0};
}

void AddHistogramView(opentelemetry::sdk::metrics::MeterProvider& provider,
                      std::vector<double> boundaries, std::string const& name,
                      std::string const& unit) {
  auto constexpr kGrpcMeterName = "grpc-c++";
  auto constexpr kGrpcSchema = "";

  auto histogram_aggregation_config = std::make_unique<
      opentelemetry::sdk::metrics::HistogramAggregationConfig>();
  histogram_aggregation_config->boundaries_ = std::move(boundaries);
  auto aggregation_config =
      std::shared_ptr<opentelemetry::sdk::metrics::AggregationConfig>(
          std::move(histogram_aggregation_config));

  auto description =
      "A view of " + name + " with custom boundaries for Bigtable";

#if OPENTELEMETRY_VERSION_MAJOR > 1 || \
    (OPENTELEMETRY_VERSION_MAJOR == 1 && OPENTELEMETRY_VERSION_MINOR >= 23)
  (void)unit;
  provider.AddView(
      opentelemetry::sdk::metrics::InstrumentSelectorFactory::Create(
          opentelemetry::sdk::metrics::InstrumentType::kHistogram, name, unit),
      opentelemetry::sdk::metrics::MeterSelectorFactory::Create(
          kGrpcMeterName, grpc::Version(), kGrpcSchema),
      opentelemetry::sdk::metrics::ViewFactory::Create(
          name, std::move(description),
          opentelemetry::sdk::metrics::AggregationType::kHistogram,
          std::move(aggregation_config)));
#elif OPENTELEMETRY_VERSION_MAJOR > 1 || \
    (OPENTELEMETRY_VERSION_MAJOR == 1 && OPENTELEMETRY_VERSION_MINOR >= 10)
  provider.AddView(
      opentelemetry::sdk::metrics::InstrumentSelectorFactory::Create(
          opentelemetry::sdk::metrics::InstrumentType::kHistogram, name, unit),
      opentelemetry::sdk::metrics::MeterSelectorFactory::Create(
          kGrpcMeterName, grpc::Version(), kGrpcSchema),
      opentelemetry::sdk::metrics::ViewFactory::Create(
          name, std::move(description), unit,
          opentelemetry::sdk::metrics::AggregationType::kHistogram,
          std::move(aggregation_config)));
#else
  (void)unit;
  provider.AddView(
      std::make_unique<opentelemetry::sdk::metrics::InstrumentSelector>(
          opentelemetry::sdk::metrics::InstrumentType::kHistogram, name),
      std::make_unique<opentelemetry::sdk::metrics::MeterSelector>(
          kGrpcMeterName, grpc::Version(), kGrpcSchema),
      std::make_unique<opentelemetry::sdk::metrics::View>(
          name, std::move(description),
          opentelemetry::sdk::metrics::AggregationType::kHistogram,
          std::move(aggregation_config)));
#endif
}

auto MakeReaderOptions(Options const& options) {
  auto reader_options =
      opentelemetry::sdk::metrics::PeriodicExportingMetricReaderOptions{};
  auto period = options.get<bigtable::experimental::GrpcMetricsPeriodOption>();
  if (period.count() > 0) {
    reader_options.export_interval_millis = period;
  } else {
    reader_options.export_interval_millis = std::chrono::seconds(60);
  }
  auto timeout =
      options.get<bigtable::experimental::GrpcMetricsExportTimeoutOption>();
  if (timeout.count() > 0) {
    reader_options.export_timeout_millis = timeout;
  } else {
    reader_options.export_timeout_millis = std::chrono::seconds(30);
  }
  return reader_options;
}

}  // namespace

std::shared_ptr<opentelemetry::metrics::MeterProvider>
MakeClientMetricsMeterProvider(
    std::unique_ptr<opentelemetry::sdk::metrics::PushMetricExporter> exporter,
    Options const& options) {
#if OPENTELEMETRY_VERSION_MAJOR > 1 || \
    (OPENTELEMETRY_VERSION_MAJOR == 1 && OPENTELEMETRY_VERSION_MINOR >= 10)
  auto provider = opentelemetry::sdk::metrics::MeterProviderFactory::Create(
      std::make_unique<opentelemetry::sdk::metrics::ViewRegistry>(),
      opentelemetry::sdk::resource::Resource::Create({}));
#else
  auto provider = std::make_unique<opentelemetry::sdk::metrics::MeterProvider>(
      std::make_unique<opentelemetry::sdk::metrics::ViewRegistry>(),
      opentelemetry::sdk::resource::Resource::Create({}));
#endif
  auto* p =
      static_cast<opentelemetry::sdk::metrics::MeterProvider*>(provider.get());
  AddHistogramView(*p, MakeLatencyHistogramBoundaries(),
                   "grpc.client.attempt.duration", "s");

  auto reader_options = MakeReaderOptions(options);
#if OPENTELEMETRY_VERSION_MAJOR > 1 || OPENTELEMETRY_VERSION_MINOR >= 10
  p->AddMetricReader(
      opentelemetry::sdk::metrics::PeriodicExportingMetricReaderFactory::Create(
          std::move(exporter), std::move(reader_options)));
#else
  p->AddMetricReader(
      std::make_unique<
          opentelemetry::sdk::metrics::PeriodicExportingMetricReader>(
          std::move(exporter), std::move(reader_options)));
#endif

  return std::shared_ptr<opentelemetry::metrics::MeterProvider>(
      std::move(provider));
}

absl::StatusOr<std::shared_ptr<grpc::experimental::OpenTelemetryPlugin>>
CreateBigtableGrpcOtelPlugin(
    std::shared_ptr<opentelemetry::metrics::MeterProvider> const& provider,
    Options const& options) {
  auto const metrics = std::vector<absl::string_view>{
      absl::string_view{"grpc.lb.wrr.rr_fallback"},
      absl::string_view{"grpc.lb.wrr.endpoint_weight_not_yet_usable"},
      absl::string_view{"grpc.lb.wrr.endpoint_weight_stale"},
      absl::string_view{"grpc.lb.wrr.endpoint_weights"},
      absl::string_view{"grpc.xds_client.connected"},
      absl::string_view{"grpc.xds_client.server_failure"},
      absl::string_view{"grpc.xds_client.resource_updates_valid"},
      absl::string_view{"grpc.xds_client.resource_updates_invalid"},
      absl::string_view{"grpc.xds_client.resources"},
      absl::string_view{"grpc.lb.rls.cache_size"},
      absl::string_view{"grpc.lb.rls.cache_entries"},
      absl::string_view{"grpc.lb.rls.default_target_picks"},
      absl::string_view{"grpc.lb.rls.target_picks"},
      absl::string_view{"grpc.lb.rls.failed_picks"},
  };

  auto authority = options.get<AuthorityOption>();
  auto scope_filter =
      [authority = std::move(authority)](
          grpc::OpenTelemetryPluginBuilder::ChannelScope const& scope) {
        return scope.default_authority() == authority;
      };

  auto disable_metrics = std::vector<absl::string_view>{
      absl::string_view("grpc.client.attempt.started"),
  };

  return grpc::OpenTelemetryPluginBuilder()
      .SetMeterProvider(provider)
      .EnableMetrics(metrics)
      .DisableMetrics(disable_metrics)
      .AddOptionalLabel(absl::string_view("grpc.lb.locality"))
      .SetGenericMethodAttributeFilter([](absl::string_view target) {
        return absl::StartsWith(target, "google.bigtable.v2");
      })
      .SetChannelScopeFilter(std::move(scope_filter))
      .Build();
}

GOOGLE_CLOUD_CPP_INLINE_NAMESPACE_END
}  // namespace bigtable_internal
}  // namespace cloud
}  // namespace google

#endif  // GOOGLE_CLOUD_CPP_BIGTABLE_WITH_OTEL_METRICS
