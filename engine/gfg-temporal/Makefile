# GFG Temporal Engine (experimental).  Builds the Vulkan compute engine, the
# te-bench CLI and the native tests.  Shaders are compiled from source at
# build time; nothing generated is committed.
CC ?= cc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Werror
GLSLANG ?= glslangValidator
SPIRV_VAL ?= spirv-val
PYTHON ?= python3
VK_CFLAGS ?= $(shell pkg-config --cflags vulkan 2>/dev/null)
BUILD ?= build
# Tests run on Mesa lavapipe (CPU) unless TE_ICD points elsewhere.
TE_ICD ?= /usr/share/vulkan/icd.d/lvp_icd.json

SHADERS := me stats classify lists reuse light full
FMT_SHADERS := luma down
LMC_SHADERS := prep down search layers hyp irls palette field masked orphan select compose sf_prepare sf_compose sf_model
FP16_SHADERS := reuse light full lmc_compose lmc_sf_compose
PQ_SHADERS := prep compose sf_compose
SPV := $(PQ_SHADERS:%=$(BUILD)/spv/lmc_%_pq.spv) $(FP16_SHADERS:%=$(BUILD)/spv/%_fp16.spv) $(SHADERS:%=$(BUILD)/spv/%.spv) \
       $(foreach s,$(FMT_SHADERS),$(BUILD)/spv/$(s)_r16f.spv $(BUILD)/spv/$(s)_r32f.spv) \
       $(LMC_SHADERS:%=$(BUILD)/spv/lmc_%.spv)
HDR := $(wildcard shaders/*.glsl)
LMC_HDR := $(wildcard shaders/lmc/*.glsl)
ENGINE := src/te_engine.c src/te_scheduler.c src/te_lmc_fit.c
ENGINE_INC := src/te_lmc.inc

.PHONY: all lib test clean
all: $(BUILD)/te-bench $(BUILD)/te-tests

# Position-independent static library for the MAKO renderer build.
lib: $(BUILD)/libgfg-temporal.a
$(BUILD)/pic/%.o: src/%.c src/*.h $(ENGINE_INC) $(BUILD)/te_shaders.h
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -fPIC -fvisibility=hidden $(VK_CFLAGS) -I$(BUILD) -Isrc -c -o $@ $<
$(BUILD)/libgfg-temporal.a: $(BUILD)/pic/te_engine.o $(BUILD)/pic/te_scheduler.o $(BUILD)/pic/te_lmc_fit.o
	$(AR) rcs $@ $^

$(BUILD)/spv/%.spv: shaders/%.comp $(HDR)
	@mkdir -p $(@D)
	$(GLSLANG) -V --target-env vulkan1.2 -o $@ $< >/dev/null
	$(SPIRV_VAL) --target-env vulkan1.2 $@

$(BUILD)/spv/lmc_%_fp16.spv: shaders/lmc/lmc_%.comp $(LMC_HDR)
	@mkdir -p $(@D)
	$(GLSLANG) -V --target-env vulkan1.2 -DOUTPUT_FMT=rgba16f -o $@ $< >/dev/null
	$(SPIRV_VAL) --target-env vulkan1.2 $@

$(BUILD)/spv/lmc_%_pq.spv: shaders/lmc/lmc_%.comp $(LMC_HDR)
	@mkdir -p $(@D)
	$(GLSLANG) -V --target-env vulkan1.2 -DGFG_LINEAR_PQ=1 -DOUTPUT_FMT=rgba16f -o $@ $< >/dev/null
	$(SPIRV_VAL) --target-env vulkan1.2 $@

$(BUILD)/spv/%_fp16.spv: shaders/%.comp $(HDR)
	@mkdir -p $(@D)
	$(GLSLANG) -V --target-env vulkan1.2 -DOUTPUT_FMT=rgba16f -o $@ $< >/dev/null
	$(SPIRV_VAL) --target-env vulkan1.2 $@

$(BUILD)/spv/%_r16f.spv: shaders/%.comp $(HDR)
	@mkdir -p $(@D)
	$(GLSLANG) -V --target-env vulkan1.2 -DLUMA_FMT=r16f -o $@ $< >/dev/null
	$(SPIRV_VAL) --target-env vulkan1.2 $@

$(BUILD)/spv/%_r32f.spv: shaders/%.comp $(HDR)
	@mkdir -p $(@D)
	$(GLSLANG) -V --target-env vulkan1.2 -DLUMA_FMT=r32f -o $@ $< >/dev/null
	$(SPIRV_VAL) --target-env vulkan1.2 $@

$(BUILD)/spv/lmc_%.spv: shaders/lmc/lmc_%.comp $(LMC_HDR)
	@mkdir -p $(@D)
	$(GLSLANG) -V --target-env vulkan1.2 -o $@ $< >/dev/null
	$(SPIRV_VAL) --target-env vulkan1.2 $@

$(BUILD)/te_shaders.h: $(SPV) tools/embed_spirv.py
	$(PYTHON) tools/embed_spirv.py $@ $(foreach f,$(SPV),$(notdir $(basename $(f)))=$(f))

$(BUILD)/te-bench: src/te_bench.c $(ENGINE) $(ENGINE_INC) src/*.h $(BUILD)/te_shaders.h
	$(CC) $(CFLAGS) $(VK_CFLAGS) -I$(BUILD) -Isrc -o $@ src/te_bench.c $(ENGINE) -ldl -lpthread -lm

$(BUILD)/te-tests: tests/te_tests.c $(ENGINE) $(ENGINE_INC) src/*.h $(BUILD)/te_shaders.h
	$(CC) $(CFLAGS) $(VK_CFLAGS) -I$(BUILD) -Isrc -o $@ tests/te_tests.c $(ENGINE) -ldl -lpthread -lm

test: $(BUILD)/te-tests $(BUILD)/te-bench
	VK_ICD_FILENAMES=$(TE_ICD) $(BUILD)/te-tests

clean:
	rm -rf $(BUILD)
