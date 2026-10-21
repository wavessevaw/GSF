# SPDX-License-Identifier: GPL-3.0-or-later
# KEDAVRA: scheduler model, interpolation engine and tools.
#
#   make            build everything into build/
#   make test       scheduler unit tests, model stress matrix, engine correctness (needs a Vulkan ICD)
#   make bench      engine GPU timing (run on the Steam Deck for real numbers)

BUILD ?= build
CC ?= cc
CFLAGS ?= -O2 -g
WARN = -Wall -Wextra -Wno-missing-field-initializers
GLSLANG ?= glslangValidator
PYTHON ?= python3

SHADERS = kfg_copy kfg_reduce kfg_me kfg_stats kfg_select kfg_warp
SPV = $(SHADERS:%=$(BUILD)/%.spv)

all: $(BUILD)/kfg_sim $(BUILD)/test_sched $(BUILD)/kfg_vktest $(SPV)

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/%.spv: shaders/%.comp shaders/kfg_common.glsl | $(BUILD)
	$(GLSLANG) -V --target-env vulkan1.1 -Ishaders -o $@ $<

$(BUILD)/kfg_sim: sched/kfg_sim.c sched/kfg_sched.h | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) -std=c11 -o $@ sched/kfg_sim.c -lm

$(BUILD)/test_sched: sched/test_sched.c sched/kfg_sched.h | $(BUILD)
	$(CC) -O1 -g -fsanitize=address,undefined $(WARN) -std=c11 -o $@ sched/test_sched.c

$(BUILD)/kfg_vktest: engine/kfg_vktest.c engine/kfg_vk.c engine/kfg_vk.h engine/kfg_plan.h | $(BUILD)
	$(CC) $(CFLAGS) $(WARN) -std=c11 -Iengine -o $@ engine/kfg_vktest.c engine/kfg_vk.c -lvulkan -lm

test-sched: $(BUILD)/test_sched $(BUILD)/kfg_sim
	$(BUILD)/test_sched
	$(PYTHON) sched/run_scenarios.py --sim $(BUILD)/kfg_sim --seeds 3 --out $(BUILD)/scenarios.json

test-engine: $(BUILD)/kfg_vktest $(SPV)
	$(BUILD)/kfg_vktest --spv $(BUILD) --json $(BUILD)/engine.json

test: test-sched test-engine

bench: $(BUILD)/kfg_vktest $(SPV)
	$(BUILD)/kfg_vktest --spv $(BUILD) --json $(BUILD)/engine-bench.json --bench 200

clean:
	rm -rf $(BUILD)

.PHONY: all test test-sched test-engine bench clean
