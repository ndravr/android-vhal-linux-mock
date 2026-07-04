#pragma once
#include "linux_vehicle_hal.pb.h"
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace virtualsdv
{
    namespace pb = vehicle::v1;
    constexpr int32_t kGlobalArea = 0;
    constexpr int32_t kRow1LeftSeat = 0x1;

    inline int64_t monotonicNanos()
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    inline std::vector<pb::VehiclePropConfig> mvpConfigs()
    {
        std::vector<pb::VehiclePropConfig> configs;
        auto addGlobal = [&](int32_t prop, const std::string &name, pb::VehiclePropertyAccess access,
                             pb::VehiclePropertyChangeMode mode, const std::string &unit) -> pb::VehiclePropConfig &
        {
            auto &c = configs.emplace_back();
            c.set_prop(prop);
            c.set_name(name);
            c.set_access(access);
            c.set_change_mode(mode);
            c.set_unit(unit);
            c.add_area_configs()->set_area_id(kGlobalArea);
            return c;
        };
        auto &speed = addGlobal(pb::PERF_VEHICLE_SPEED, "PERF_VEHICLE_SPEED", pb::VEHICLE_PROPERTY_ACCESS_READ,
                                pb::VEHICLE_PROPERTY_CHANGE_MODE_CONTINUOUS, "m/s");
        speed.set_min_sample_rate(1.0F);
        speed.set_max_sample_rate(10.0F);
        auto &gear = addGlobal(pb::GEAR_SELECTION, "GEAR_SELECTION", pb::VEHICLE_PROPERTY_ACCESS_READ,
                               pb::VEHICLE_PROPERTY_CHANGE_MODE_ON_CHANGE, "enum");
        for (int value : {0, 1, 2, 4, 8})
            gear.mutable_area_configs(0)->add_supported_enum_values(value);
        auto &ignition = addGlobal(pb::IGNITION_STATE, "IGNITION_STATE", pb::VEHICLE_PROPERTY_ACCESS_READ,
                                   pb::VEHICLE_PROPERTY_CHANGE_MODE_ON_CHANGE, "enum");
        for (int value : {0, 2, 3, 4})
            ignition.mutable_area_configs(0)->add_supported_enum_values(value);
        auto &hvac = configs.emplace_back();
        hvac.set_prop(pb::HVAC_TEMPERATURE_SET);
        hvac.set_name("HVAC_TEMPERATURE_SET");
        hvac.set_access(pb::VEHICLE_PROPERTY_ACCESS_READ_WRITE);
        hvac.set_change_mode(pb::VEHICLE_PROPERTY_CHANGE_MODE_ON_CHANGE);
        hvac.set_unit("degC");
        auto *area = hvac.add_area_configs();
        area->set_area_id(kRow1LeftSeat);
        area->set_min_float_value(16.0F);
        area->set_max_float_value(30.0F);
        return configs;
    }

    inline std::optional<pb::VehiclePropConfig> findConfig(int32_t prop)
    {
        for (const auto &c : mvpConfigs())
            if (c.prop() == prop)
                return c;
        return std::nullopt;
    }
    inline int32_t propFromName(const std::string &name)
    {
        for (const auto &c : mvpConfigs())
            if (c.name() == name)
                return c.prop();
        try
        {
            return static_cast<int32_t>(std::stoul(name, nullptr, 0));
        }
        catch (...)
        {
            return 0;
        }
    }
    inline std::string propName(int32_t prop)
    {
        auto c = findConfig(prop);
        return c ? c->name() : "UNKNOWN_PROPERTY";
    }
} // namespace virtualsdv
