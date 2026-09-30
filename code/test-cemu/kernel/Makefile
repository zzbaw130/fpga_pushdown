BUILD_DIR := ../build
CSD_SOURCES = csd_sql.cc
CSD_OPERATOR_FILES = csd_fifo.cc csd_scan.cc csd_expression.cc csd_filter.cc csd_project.cc csd_aggregate.cc \
	csd_join.cc csd_sort.cc csd_control.cc
TARGETS = $(BUILD_DIR)/csd_sql.so $(BUILD_DIR)/csd_sql_test

CXX = g++
CXXFLAGS = -Wall -Wextra -O2 -march=native -std=c++11 -pthread

.PHONY: all test clean

all: $(BUILD_DIR) $(TARGETS)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/csd_sql.so: $(BUILD_DIR) $(CSD_SOURCES) $(CSD_OPERATOR_FILES) csd_plan.h cemu_def.h
	$(CXX) $(CXXFLAGS) -shared -fPIC -o $@ $(CSD_SOURCES)

$(BUILD_DIR)/csd_sql_test: $(BUILD_DIR) csd_sql_test.cc $(CSD_SOURCES) $(CSD_OPERATOR_FILES) csd_plan.h cemu_def.h
	$(CXX) $(CXXFLAGS) -o $@ csd_sql_test.cc $(CSD_SOURCES)

test: $(BUILD_DIR)/csd_sql_test
	$(BUILD_DIR)/csd_sql_test

clean:
	rm -f $(TARGETS)
