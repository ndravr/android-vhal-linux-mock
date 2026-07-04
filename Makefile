PROJECT_DIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
PROTO := $(PROJECT_DIR)/proto/linux_vehicle_hal.proto
SRC := $(PROJECT_DIR)/src
TESTS := $(PROJECT_DIR)/tests
BUILD := $(PROJECT_DIR)/build
GEN := $(BUILD)/generated
ifeq ($(V),1)
Q :=
else
Q := @
endif
CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -Wpedantic -pthread -I$(GEN) -I$(SRC) $(shell pkg-config --cflags grpc++ protobuf)
LDLIBS := $(shell pkg-config --libs grpc++ protobuf) -pthread
GENERATED := $(GEN)/linux_vehicle_hal.pb.cc $(GEN)/linux_vehicle_hal.grpc.pb.cc
GENERATED_STAMP := $(GEN)/.generated
COMMON_OBJECTS := $(BUILD)/linux_vehicle_hal.pb.o $(BUILD)/linux_vehicle_hal.grpc.pb.o
.PHONY: all clean smoke
all: $(BUILD)/linux_vhal_stub $(BUILD)/qnx_provider_sim $(BUILD)/vehicle_client
$(GENERATED_STAMP): $(PROTO)
	@printf '  %-7s %s\n' PROTO $(notdir $(PROTO))
	$(Q)mkdir -p $(GEN)
	$(Q)protoc -I$(dir $(PROTO)) --cpp_out=$(GEN) --grpc_out=$(GEN) --plugin=protoc-gen-grpc=$$(command -v grpc_cpp_plugin) $(PROTO)
	$(Q)touch $@
$(GENERATED): $(GENERATED_STAMP)
$(BUILD)/linux_vehicle_hal.pb.o: $(GEN)/linux_vehicle_hal.pb.cc
	@printf '  %-7s %s\n' CXX $(notdir $<)
	$(Q)$(CXX) $(CXXFLAGS) -c $< -o $@
$(BUILD)/linux_vehicle_hal.grpc.pb.o: $(GEN)/linux_vehicle_hal.grpc.pb.cc
	@printf '  %-7s %s\n' CXX $(notdir $<)
	$(Q)$(CXX) $(CXXFLAGS) -c $< -o $@
$(BUILD)/linux_vhal_stub: $(SRC)/vhal_stub.cpp $(SRC)/vhal_common.h $(COMMON_OBJECTS)
	@printf '  %-7s %s\n' LINK $(notdir $@)
	$(Q)$(CXX) $(CXXFLAGS) $< $(COMMON_OBJECTS) $(LDLIBS) -o $@
$(BUILD)/qnx_provider_sim: $(SRC)/provider_sim.cpp $(SRC)/vhal_common.h $(COMMON_OBJECTS)
	@printf '  %-7s %s\n' LINK $(notdir $@)
	$(Q)$(CXX) $(CXXFLAGS) $< $(COMMON_OBJECTS) $(LDLIBS) -o $@
$(BUILD)/vehicle_client: $(SRC)/vehicle_client.cpp $(SRC)/vhal_common.h $(COMMON_OBJECTS)
	@printf '  %-7s %s\n' LINK $(notdir $@)
	$(Q)$(CXX) $(CXXFLAGS) $< $(COMMON_OBJECTS) $(LDLIBS) -o $@
clean:
	@printf '  %-7s %s\n' CLEAN $(notdir $(BUILD))
	$(Q)rm -rf $(BUILD)
smoke: all
	@printf '  %-7s %s\n' TEST smoke
	$(Q)bash $(TESTS)/smoke.sh
