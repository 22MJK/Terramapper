CXX ?= clang++
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra -Wpedantic -I.

BUILD_DIR := build
OBJ := \
	$(BUILD_DIR)/topology.o \
	$(BUILD_DIR)/graph.o \
	$(BUILD_DIR)/mapper.o \
	$(BUILD_DIR)/strategies.o \
	$(BUILD_DIR)/scheduler.o \
	$(BUILD_DIR)/trace.o \
	$(BUILD_DIR)/workload.o \
	$(BUILD_DIR)/main.o

all: mapper_demo

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

mapper_demo: $(OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJ)

$(BUILD_DIR)/topology.o: hardware_topology/topology.cpp hardware_topology/topology.h | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c -o $@ hardware_topology/topology.cpp

$(BUILD_DIR)/graph.o: mapping/graph.cpp mapping/graph.h | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c -o $@ mapping/graph.cpp

$(BUILD_DIR)/mapper.o: mapping/mapper.cpp mapping/mapper.h mapping/graph.h hardware_topology/topology.h | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c -o $@ mapping/mapper.cpp

$(BUILD_DIR)/strategies.o: mapping/strategies.cpp mapping/strategies.h mapping/graph.h | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c -o $@ mapping/strategies.cpp

$(BUILD_DIR)/scheduler.o: schedule/scheduler.cpp schedule/scheduler.h mapping/graph.h mapping/mapper.h hardware_topology/topology.h | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c -o $@ schedule/scheduler.cpp

$(BUILD_DIR)/trace.o: trace_generator/trace.cpp trace_generator/trace.h schedule/scheduler.h | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c -o $@ trace_generator/trace.cpp

$(BUILD_DIR)/workload.o: workload/workload.cpp workload/workload.h mapping/graph.h | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c -o $@ workload/workload.cpp

$(BUILD_DIR)/main.o: main.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c -o $@ main.cpp

clean:
	rm -rf $(BUILD_DIR) mapper_demo

.PHONY: all clean
