CXX ?= g++
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra -pedantic
CPPFLAGS ?= -Iinclude

# build/ 下只放编译产物；src/ 下按职责分层，object 文件保持相同子目录结构。
BUILD_DIR := build
TARGET := $(BUILD_DIR)/hbm_sim
SEQUENCE_TEST := $(BUILD_DIR)/sequence_tests
TIMING_BOUNDARY_TEST := $(BUILD_DIR)/timing_boundary_tests
# 主程序需要 src/cli/main.cpp；测试程序需要复用库代码但不能链接 CLI main。
SRCS := $(shell find src -name '*.cpp' | sort)
LIB_SRCS := $(filter-out src/cli/main.cpp,$(SRCS))
OBJS := $(patsubst src/%.cpp,$(BUILD_DIR)/%.o,$(SRCS))
LIB_OBJS := $(patsubst src/%.cpp,$(BUILD_DIR)/%.o,$(LIB_SRCS))
DEPS := $(OBJS:.o=.d)

.PHONY: all clean clean-outputs delete run test smoke sequence-test timing-boundary-validation \
	model-validation performance-validation sensitivity-validation reference-validation ramulator-validation \
	examples examples_hbm4 examples_hbm3 examples_lpddr6 examples_lpddr5

RAMULATOR2_ROOT ?= /path/to/ramulator2

all: $(TARGET)

# 链接单个命令行工具。项目没有外部依赖，因此只需要把所有 object 合并即可。
$(TARGET): $(OBJS) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(OBJS) -o $@

# 每个 src/**/*.cpp 独立编译，方便增量构建；object 目录镜像源码分层。
$(BUILD_DIR)/%.o: src/%.cpp | $(BUILD_DIR)
	mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c $< -o $@

# sequence_tests 直接链接控制器库对象，用 C++ assert-style 测试检查协议序列。
$(SEQUENCE_TEST): $(LIB_OBJS) tests/sequence_tests.cpp | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(LIB_OBJS) tests/sequence_tests.cpp -o $@

$(TIMING_BOUNDARY_TEST): $(LIB_OBJS) tests/timing_boundary_tests.cpp | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(LIB_OBJS) tests/timing_boundary_tests.cpp -o $@

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

run: $(TARGET)
	./$(TARGET) --standard hbm4 --pattern stream --requests 10000

# examples/test 都显式用 bash 运行，避免依赖脚本文件执行位。
examples: $(TARGET)
	bash ./examples/run_hbm4_stream.sh
	bash ./examples/run_hbm3_random.sh
	bash ./examples/run_lpddr6_trace.sh
	bash ./examples/run_lpddr5_trace.sh

examples_hbm4: $(TARGET)
	bash ./examples/run_hbm4_stream.sh
examples_hbm3: $(TARGET)
	bash ./examples/run_hbm3_random.sh
examples_lpddr6: $(TARGET)
	bash ./examples/run_lpddr6_trace.sh
examples_lpddr5: $(TARGET)
	bash ./examples/run_lpddr5_trace.sh

smoke: $(TARGET)
	bash ./tests/smoke.sh

sequence-test: $(SEQUENCE_TEST)
	./$(SEQUENCE_TEST)

timing-boundary-validation: $(TIMING_BOUNDARY_TEST)
	python3 ./tools/timing_boundary_validation.py --probe ./$(TIMING_BOUNDARY_TEST)

model-validation: $(TARGET)
	python3 ./tools/model_validation.py --binary ./$(TARGET)

performance-validation: $(TARGET)
	python3 ./tools/performance_curve.py --binary ./$(TARGET)

sensitivity-validation: $(TARGET)
	python3 ./tools/sensitivity_uncertainty.py --binary ./$(TARGET)

reference-validation: $(TARGET)
	python3 ./tools/ramulator2_differential.py --binary ./$(TARGET) \
		--ramulator-root "$(RAMULATOR2_ROOT)"

# Compatibility alias. The primary name emphasizes that Ramulator is only an
# external reference for the shared command surface.
ramulator-validation: reference-validation

# test 同时跑 smoke 和 sequence-test：
# - smoke 关注 CLI 主路径和输出字段
# - sequence-test 关注精确命令顺序和 timing 间隔
# - model-validation 关注理论公式、DFI、来源审计和敏感性阈值
test: smoke sequence-test timing-boundary-validation model-validation

clean:
	rm -rf $(BUILD_DIR)

# 仿真产物统一放在 outputs/。该目标保留 outputs/README.md，只删除其余产物；
# 下一次 CLI 运行会自动重建所需子目录。
clean-outputs:
	@echo "Deleting simulation outputs under outputs/..."
	@mkdir -p outputs
	@find outputs -depth -mindepth 1 ! -path 'outputs/README.md' -print -delete

# 兼容旧命令；delete 与 clean-outputs 语义完全相同。
delete: clean-outputs

-include $(DEPS)
