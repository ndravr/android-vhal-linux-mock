#include "linux_vehicle_hal.grpc.pb.h"
#include "vhal_common.h"
#include <grpcpp/grpcpp.h>
#include <atomic>
#include <condition_variable>
#include <iostream>
#include <map>
#include <mutex>
#include <thread>

namespace virtualsdv
{
    namespace
    {
        class ProviderService final : public pb::CanonicalVehicleProvider::Service
        {
        public:
            ProviderService()
            {
                initialize(pb::CANONICAL_VEHICLE_SPEED_KPH, 0.0, 0);
                initialize(pb::CANONICAL_GEAR_POSITION, 0.0, 1);  // P
                initialize(pb::CANONICAL_IGNITION_STATE, 0.0, 1); // OFF
                initialize(pb::CANONICAL_HVAC_TARGET_TEMPERATURE_C, 22.0, 0);
                demo_ = std::thread([this]
                                    { demoLoop(); });
            }
            ~ProviderService() override
            {
                stop_ = true;
                cv_.notify_all();
                if (demo_.joinable())
                    demo_.join();
            }

            grpc::Status GetSnapshot(grpc::ServerContext *, const pb::Empty *, pb::CanonicalSignalValues *out) override
            {
                std::lock_guard lock(mutex_);
                for (const auto &item : values_)
                    *out->add_values() = item.second;
                return grpc::Status::OK;
            }
            grpc::Status SetSignal(grpc::ServerContext *, const pb::SetSignalRequest *request, pb::SetSignalResult *out) override
            {
                out->set_request_id(request->request_id());
                if (request->value().signal() != pb::CANONICAL_HVAC_TARGET_TEMPERATURE_C)
                {
                    out->set_status(pb::STATUS_ACCESS_DENIED);
                    out->set_message("signal is read-only");
                    return grpc::Status::OK;
                }
                const double value = request->value().float_value();
                if (value < 16.0 || value > 30.0)
                {
                    out->set_status(pb::STATUS_INVALID_ARG);
                    out->set_message("HVAC temperature must be in [16, 30] degC");
                    return grpc::Status::OK;
                }
                publish(pb::CANONICAL_HVAC_TARGET_TEMPERATURE_C, value, 0);
                out->set_status(pb::STATUS_OK);
                out->set_message("committed by QNX authority simulator");
                return grpc::Status::OK;
            }
            grpc::Status Subscribe(grpc::ServerContext *context, const pb::Empty *, grpc::ServerWriter<pb::CanonicalSignalValues> *writer) override
            {
                size_t cursor;
                {
                    std::lock_guard lock(mutex_);
                    cursor = events_.size();
                }
                while (!context->IsCancelled() && !stop_)
                {
                    pb::CanonicalSignalValue value;
                    {
                        std::unique_lock lock(mutex_);
                        cv_.wait_for(lock, std::chrono::milliseconds(250), [&]
                                     { return stop_ || cursor < events_.size(); });
                        if (stop_ || context->IsCancelled())
                            break;
                        if (cursor >= events_.size())
                            continue;
                        value = events_[cursor++];
                    }
                    pb::CanonicalSignalValues batch;
                    *batch.add_values() = value;
                    if (!writer->Write(batch))
                        break;
                }
                return grpc::Status::OK;
            }

        private:
            void initialize(pb::CanonicalSignal signal, double number, int enumeration)
            {
                pb::CanonicalSignalValue value;
                value.set_signal(signal);
                value.set_float_value(number);
                value.set_enum_value(enumeration);
                value.set_source_timestamp(monotonicNanos());
                value.set_source_sequence(++sequence_);
                values_[signal] = value;
            }
            void publish(pb::CanonicalSignal signal, double number, int enumeration)
            {
                std::lock_guard lock(mutex_);
                auto &value = values_[signal];
                value.set_signal(signal);
                value.set_float_value(number);
                value.set_enum_value(enumeration);
                value.set_source_timestamp(monotonicNanos());
                value.set_source_sequence(++sequence_);
                events_.push_back(value);
                cv_.notify_all();
            }
            void demoLoop()
            {
                double speed = 0.0;
                while (!stop_)
                {
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                    if (stop_)
                        break;
                    speed = speed >= 60.0 ? 0.0 : speed + 5.0;
                    publish(pb::CANONICAL_VEHICLE_SPEED_KPH, speed, 0);
                }
            }
            std::atomic_bool stop_{false};
            std::mutex mutex_;
            std::condition_variable cv_;
            std::map<pb::CanonicalSignal, pb::CanonicalSignalValue> values_;
            std::vector<pb::CanonicalSignalValue> events_;
            uint64_t sequence_{0};
            std::thread demo_;
        };
    } // namespace
} // namespace virtualsdv

int main(int argc, char **argv)
{
    const std::string address = argc > 1 ? argv[1] : "127.0.0.1:50051";
    virtualsdv::ProviderService service;
    grpc::ServerBuilder builder;
    builder.AddListeningPort(address, grpc::InsecureServerCredentials());
    builder.RegisterService(&service);
    auto server = builder.BuildAndStart();
    if (!server)
    {
        std::cerr << "Failed to listen on " << address << '\n';
        return 1;
    }
    std::cout << "QNX authority simulator listening on " << address << std::endl;
    server->Wait();
}
