#include "linux_vehicle_hal.grpc.pb.h"
#include "vhal_common.h"
#include <grpcpp/grpcpp.h>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <stdexcept>

namespace virtualsdv
{
    namespace
    {
        void printValue(const pb::VehiclePropValue &value)
        {
            std::cout << propName(value.prop()) << " area=" << value.area_id() << " value=";
            if (value.value().float_values_size())
                std::cout << value.value().float_values(0);
            else if (value.value().int32_values_size())
                std::cout << value.value().int32_values(0);
            else
                std::cout << "<empty>";
            std::cout << " status=" << pb::VehiclePropertyStatus_Name(value.status()) << " sequence=" << value.source_sequence() << '\n';
        }

        int run(int argc, char **argv)
        {
            if (argc < 2)
            {
                std::cerr << "Usage: vehicle_client <configs|get|set|subscribe|health> [property] [value]\n";
                return 2;
            }
            const char *env = std::getenv("VHAL_SOCKET");
            const std::string socket = env ? env : "/tmp/virtual-sdv-vhal.sock";
            auto stub = pb::LinuxVehicleHal::NewStub(grpc::CreateChannel("unix:" + socket, grpc::InsecureChannelCredentials()));
            const std::string command = argv[1];
            if (command == "configs")
            {
                grpc::ClientContext context;
                pb::Empty request;
                pb::VehiclePropConfigs response;
                auto status = stub->GetAllPropConfigs(&context, request, &response);
                if (!status.ok())
                    throw std::runtime_error(status.error_message());
                for (const auto &c : response.configs())
                    std::cout << c.name() << " id=0x" << std::hex << c.prop() << std::dec
                              << " access=" << pb::VehiclePropertyAccess_Name(c.access()) << " mode=" << pb::VehiclePropertyChangeMode_Name(c.change_mode()) << " unit=" << c.unit() << '\n';
                return 0;
            }
            if (command == "health")
            {
                grpc::ClientContext context;
                pb::Empty request;
                pb::VehicleHalHealth response;
                auto status = stub->GetHealth(&context, request, &response);
                if (!status.ok())
                    throw std::runtime_error(status.error_message());
                std::cout << response.state() << " provider_connected=" << std::boolalpha << response.provider_connected() << '\n';
                return response.provider_connected() ? 0 : 1;
            }
            if (argc < 3)
                throw std::runtime_error("property name is required");
            const int32_t prop = propFromName(argv[2]);
            const auto config = findConfig(prop);
            if (!config)
                throw std::runtime_error("unknown property: " + std::string(argv[2]));
            const int area = config->area_configs_size() ? config->area_configs(0).area_id() : 0;
            if (command == "get")
            {
                pb::GetValueRequests request;
                auto *item = request.add_requests();
                item->set_request_id(1);
                item->set_prop(prop);
                item->set_area_id(area);
                pb::GetValueResults response;
                grpc::ClientContext context;
                auto status = stub->GetValues(&context, request, &response);
                if (!status.ok())
                    throw std::runtime_error(status.error_message());
                if (!response.results_size() || response.results(0).status() != pb::STATUS_OK)
                {
                    std::cerr << "get failed: " << pb::StatusCode_Name(response.results(0).status()) << '\n';
                    return 1;
                }
                printValue(response.results(0).value());
                return 0;
            }
            if (command == "set")
            {
                if (argc < 4)
                    throw std::runtime_error("value is required");
                pb::SetValueRequests request;
                auto *item = request.add_requests();
                item->set_request_id(1);
                item->mutable_value()->set_prop(prop);
                item->mutable_value()->set_area_id(area);
                item->mutable_value()->mutable_value()->add_float_values(std::stof(argv[3]));
                pb::SetValueResults response;
                grpc::ClientContext context;
                auto status = stub->SetValues(&context, request, &response);
                if (!status.ok())
                    throw std::runtime_error(status.error_message());
                std::cout << pb::StatusCode_Name(response.results(0).status()) << ": " << response.results(0).message() << '\n';
                return response.results(0).status() == pb::STATUS_OK ? 0 : 1;
            }
            if (command == "subscribe")
            {
                pb::SubscribeRequest request;
                request.add_prop_ids(prop);
                grpc::ClientContext context;
                auto reader = stub->Subscribe(&context, request);
                pb::VehiclePropValues batch;
                while (reader->Read(&batch))
                    for (const auto &value : batch.values())
                        printValue(value);
                return reader->Finish().ok() ? 0 : 1;
            }
            throw std::runtime_error("unknown command: " + command);
        }
    } // namespace
} // namespace virtualsdv

int main(int argc, char **argv)
{
    try
    {
        return virtualsdv::run(argc, argv);
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << '\n';
        return 2;
    }
}
