CXX ?= g++
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra -pedantic
CPPFLAGS ?= -Iinclude

# build/ 下只放编译产物；src/ 下按职责分层，object 文件保持相同子目录结构。
BUILD_DIR := build
TARGET := $(BUILD_DIR)/hbm_sim
SEQUENCE_TEST := $(BUILD_DIR)/sequence_tests
TIMING_BOUNDARY_TEST := $(BUILD_DIR)/timing_boundary_tests
PHY_TEST := $(BUILD_DIR)/phy_tests
CONFIG_TEST := $(BUILD_DIR)/config_tests
# 主程序需要 src/cli/main.cpp；测试程序需要复用库代码但不能链接 CLI main。
SRCS := $(shell find src -name '*.cpp' | sort)
LIB_SRCS := $(filter-out src/cli/main.cpp,$(SRCS))
OBJS := $(patsubst src/%.cpp,$(BUILD_DIR)/%.o,$(SRCS))
LIB_OBJS := $(patsubst src/%.cpp,$(BUILD_DIR)/%.o,$(LIB_SRCS))
DEPS := $(OBJS:.o=.d)

.PHONY: all clean clean-outputs delete run test smoke sequence-test phy-test config-test phy-smoke visualization-smoke multistack-backend-smoke multistack-demos-smoke architecture-sweep-smoke architecture-sweep visualize-example timing-boundary-validation \
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

$(PHY_TEST): $(LIB_OBJS) tests/phy_tests.cpp | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(LIB_OBJS) tests/phy_tests.cpp -o $@

$(CONFIG_TEST): $(LIB_OBJS) tests/config_tests.cpp | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(LIB_OBJS) tests/config_tests.cpp -o $@

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

run: $(TARGET)
	./$(TARGET) --config configs/hbm.cfg --standard hbm4 --preset baseline \
		--pattern stream --requests 10000

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

phy-test: $(PHY_TEST)
	./$(PHY_TEST)

config-test: $(CONFIG_TEST)
	./$(CONFIG_TEST) .

phy-smoke: $(TARGET)
	HBM_SIM_BIN=./$(TARGET) HBM_SIM_SOURCE_DIR=. bash ./tests/phy_smoke.sh

visualization-smoke: $(TARGET)
	HBM_SIM_BIN=./$(TARGET) HBM_SIM_SOURCE_DIR=. bash ./tests/visualization_smoke.sh

multistack-backend-smoke: $(TARGET)
	HBM_SIM_BIN=./$(TARGET) HBM_SIM_SOURCE_DIR=. bash ./tests/multistack_backend_smoke.sh

multistack-demos-smoke: $(TARGET)
	HBM_SIM_BIN=./$(TARGET) HBM_SIM_SOURCE_DIR=. bash ./tests/multistack_demos_smoke.sh

architecture-sweep-smoke: $(TARGET)
	HBM_SIM_BIN=./$(TARGET) HBM_SIM_SOURCE_DIR=. bash ./tests/architecture_sweep_smoke.sh

architecture-sweep: $(TARGET)
	python3 ./experiments/architecture_sweep/run.py --binary ./$(TARGET)

visualize-example: $(TARGET)
	HBM_SIM_BIN=./$(TARGET) HBM_SIM_SOURCE_DIR=. bash ./tools/visualize_example.sh

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

# Make 兼容验收覆盖核心单元、CLI、配置、可视化、多 Stack/后端和架构实验。
# 性能/敏感性 sweep 与外部 Ramulator 参考仍是显式目标，避免默认验收意外拉长。
test: smoke sequence-test phy-test config-test phy-smoke visualization-smoke \
	multistack-backend-smoke multistack-demos-smoke architecture-sweep-smoke \
	timing-boundary-validation model-validation

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
