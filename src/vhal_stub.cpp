#include "linux_vehicle_hal.grpc.pb.h"
#include "vhal_common.h"
#include <grpcpp/grpcpp.h>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <thread>

namespace virtualsdv
{
    namespace
    {
        struct Key
        {
            int32_t prop;
            int32_t area;
            bool operator<(const Key &rhs) const { return prop < rhs.prop || (prop == rhs.prop && area < rhs.area); }
        };

        class VhalService final : public pb::LinuxVehicleHal::Service
        {
        public:
            explicit VhalService(const std::string &provider) : provider_(pb::CanonicalVehicleProvider::NewStub(grpc::CreateChannel(provider, grpc::InsecureChannelCredentials())))
            {
                providerThread_ = std::thread([this]
                                              { providerLoop(); });
            }
            ~VhalService() override
            {
                stopping_ = true;
                {
                    std::lock_guard lock(providerContextMutex_);
                    if (activeProviderContext_)
                        activeProviderContext_->TryCancel();
                }
                cv_.notify_all();
                if (providerThread_.joinable())
                    providerThread_.join();
            }

            grpc::Status GetAllPropConfigs(grpc::ServerContext *, const pb::Empty *, pb::VehiclePropConfigs *out) override
            {
                for (const auto &config : mvpConfigs())
                    *out->add_configs() = config;
                return grpc::Status::OK;
            }
            grpc::Status GetValues(grpc::ServerContext *, const pb::GetValueRequests *requests, pb::GetValueResults *out) override
            {
                std::lock_guard lock(mutex_);
                std::set<std::pair<int32_t, int32_t>> seen;
                for (const auto &request : requests->requests())
                {
                    auto *result = out->add_results();
                    result->set_request_id(request.request_id());
                    if (!seen.emplace(request.prop(), request.area_id()).second || !findConfig(request.prop()))
                    {
                        result->set_status(pb::STATUS_INVALID_ARG);
                        continue;
                    }
                    auto found = values_.find({request.prop(), request.area_id()});
                    if (found == values_.end())
                    {
                        result->set_status(pb::STATUS_TRY_AGAIN);
                        continue;
                    }
                    result->set_status(pb::STATUS_OK);
                    *result->mutable_value() = found->second;
                }
                return grpc::Status::OK;
            }
            grpc::Status SetValues(grpc::ServerContext *, const pb::SetValueRequests *requests, pb::SetValueResults *out) override
            {
                for (const auto &request : requests->requests())
                {
                    auto *result = out->add_results();
                    result->set_request_id(request.request_id());
                    const auto config = findConfig(request.value().prop());
                    if (!config)
                    {
                        result->set_status(pb::STATUS_INVALID_ARG);
                        result->set_message("unknown property");
                        continue;
                    }
                    if (config->access() != pb::VEHICLE_PROPERTY_ACCESS_WRITE && config->access() != pb::VEHICLE_PROPERTY_ACCESS_READ_WRITE)
                    {
                        result->set_status(pb::STATUS_ACCESS_DENIED);
                        result->set_message("property is read-only");
                        continue;
                    }
                    if (request.value().prop() != pb::HVAC_TEMPERATURE_SET || request.value().area_id() != kRow1LeftSeat || request.value().value().float_values_size() != 1)
                    {
                        result->set_status(pb::STATUS_INVALID_ARG);
                        result->set_message("invalid HVAC value or area");
                        continue;
                    }
                    pb::SetSignalRequest providerRequest;
                    providerRequest.set_request_id(request.request_id());
                    providerRequest.mutable_value()->set_signal(pb::CANONICAL_HVAC_TARGET_TEMPERATURE_C);
                    providerRequest.mutable_value()->set_float_value(request.value().value().float_values(0));
                    pb::SetSignalResult providerResult;
                    grpc::ClientContext context;
                    const auto status = provider_->SetSignal(&context, providerRequest, &providerResult);
                    if (!status.ok())
                    {
                        result->set_status(pb::STATUS_NOT_AVAILABLE);
                        result->set_message("QNX provider unavailable: " + status.error_message());
                        continue;
                    }
                    result->set_status(providerResult.status());
                    result->set_message(providerResult.message());
                }
                return grpc::Status::OK;
            }
            grpc::Status Subscribe(grpc::ServerContext *context, const pb::SubscribeRequest *request, grpc::ServerWriter<pb::VehiclePropValues> *writer) override
            {
                const std::set<int32_t> selected(request->prop_ids().begin(), request->prop_ids().end());
                size_t cursor;
                {
                    std::lock_guard lock(mutex_);
                    pb::VehiclePropValues snapshot;
                    for (const auto &item : values_)
                        if (selected.empty() || selected.count(item.first.prop))
                            *snapshot.add_values() = item.second;
                    if (snapshot.values_size() && !writer->Write(snapshot))
                        return grpc::Status::OK;
                    cursor = events_.size();
                }
                while (!context->IsCancelled() && !stopping_)
                {
                    pb::VehiclePropValue value;
                    {
                        std::unique_lock lock(mutex_);
                        cv_.wait_for(lock, std::chrono::milliseconds(250), [&]
                                     { return stopping_ || cursor < events_.size(); });
                        if (stopping_ || context->IsCancelled())
                            break;
                        if (cursor >= events_.size())
                            continue;
                        value = events_[cursor++];
                    }
                    if (!selected.empty() && !selected.count(value.prop()))
                        continue;
                    pb::VehiclePropValues batch;
                    *batch.add_values() = value;
                    if (!writer->Write(batch))
                        break;
                }
                return grpc::Status::OK;
            }
            grpc::Status GetHealth(grpc::ServerContext *, const pb::Empty *, pb::VehicleHalHealth *out) override
            {
                out->set_provider_connected(connected_);
                out->set_state(connected_ ? "HEALTHY" : "DISCONNECTED");
                return grpc::Status::OK;
            }

        private:
            pb::VehiclePropValue mapSignal(const pb::CanonicalSignalValue &signal)
            {
                pb::VehiclePropValue value;
                value.set_timestamp(signal.source_timestamp());
                value.set_status(pb::VEHICLE_PROPERTY_STATUS_AVAILABLE);
                value.set_source_sequence(signal.source_sequence());
                switch (signal.signal())
                {
                case pb::CANONICAL_VEHICLE_SPEED_KPH:
                    value.set_prop(pb::PERF_VEHICLE_SPEED);
                    value.set_area_id(kGlobalArea);
                    value.mutable_value()->add_float_values(signal.float_value() / 3.6);
                    break;
                case pb::CANONICAL_GEAR_POSITION:
                {
                    const int map[] = {0, 4, 2, 1, 8};
                    const int i = signal.enum_value();
                    value.set_prop(pb::GEAR_SELECTION);
                    value.set_area_id(kGlobalArea);
                    value.mutable_value()->add_int32_values(i >= 0 && i < 5 ? map[i] : 0);
                    break;
                }
                case pb::CANONICAL_IGNITION_STATE:
                {
                    const int map[] = {0, 2, 3, 4};
                    const int i = signal.enum_value();
                    value.set_prop(pb::IGNITION_STATE);
                    value.set_area_id(kGlobalArea);
                    value.mutable_value()->add_int32_values(i >= 0 && i < 4 ? map[i] : 0);
                    break;
                }
                case pb::CANONICAL_HVAC_TARGET_TEMPERATURE_C:
                    value.set_prop(pb::HVAC_TEMPERATURE_SET);
                    value.set_area_id(kRow1LeftSeat);
                    value.mutable_value()->add_float_values(signal.float_value());
                    break;
                default:
                    value.set_prop(pb::VEHICLE_PROPERTY_INVALID);
                }
                return value;
            }
            void apply(const pb::CanonicalSignalValues &updates)
            {
                std::lock_guard lock(mutex_);
                for (const auto &signal : updates.values())
                {
                    auto value = mapSignal(signal);
                    if (!value.prop())
                        continue;
                    values_[{value.prop(), value.area_id()}] = value;
                    events_.push_back(value);
                }
                cv_.notify_all();
            }
            void providerLoop()
            {
                while (!stopping_)
                {
                    pb::Empty empty;
                    pb::CanonicalSignalValues snapshot;
                    grpc::ClientContext snapshotContext;
                    if (!provider_->GetSnapshot(&snapshotContext, empty, &snapshot).ok())
                    {
                        connected_ = false;
                        std::this_thread::sleep_for(std::chrono::seconds(1));
                        continue;
                    }
                    apply(snapshot);
                    connected_ = true;
                    grpc::ClientContext streamContext;
                    {
                        std::lock_guard lock(providerContextMutex_);
                        activeProviderContext_ = &streamContext;
                    }
                    auto reader = provider_->Subscribe(&streamContext, empty);
                    pb::CanonicalSignalValues update;
                    while (!stopping_ && reader->Read(&update))
                        apply(update);
                    reader->Finish();
                    {
                        std::lock_guard lock(providerContextMutex_);
                        activeProviderContext_ = nullptr;
                    }
                    connected_ = false;
                    if (!stopping_)
                        std::this_thread::sleep_for(std::chrono::seconds(1));
                }
            }
            std::unique_ptr<pb::CanonicalVehicleProvider::Stub> provider_;
            std::mutex providerContextMutex_;
            grpc::ClientContext *activeProviderContext_{nullptr};
            std::atomic_bool connected_{false}, stopping_{false};
            std::thread providerThread_;
            std::mutex mutex_;
            std::condition_variable cv_;
            std::map<Key, pb::VehiclePropValue> values_;
            std::vector<pb::VehiclePropValue> events_;
        };
    } // namespace
} // namespace virtualsdv

int main(int argc, char **argv)
{
    const std::string socket = argc > 1 ? argv[1] : "/tmp/virtual-sdv-vhal.sock";
    const std::string provider = argc > 2 ? argv[2] : "127.0.0.1:50051";
    std::remove(socket.c_str());
    virtualsdv::VhalService service(provider);
    grpc::ServerBuilder builder;
    builder.AddListeningPort("unix:" + socket, grpc::InsecureServerCredentials());
    builder.RegisterService(&service);
    auto server = builder.BuildAndStart();
    if (!server)
    {
        std::cerr << "Failed to listen on unix:" << socket << '\n';
        return 1;
    }
    std::cout << "Linux VHAL stub listening on unix:" << socket << "; QNX provider=" << provider << std::endl;
    server->Wait();
}
