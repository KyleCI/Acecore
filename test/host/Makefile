SRC := ../../lib/pc_core/src
CODEC := ../../src/ds3231_time.cpp ../../src/lagos_time.cpp ../../src/battery_alert_policy.cpp \
	../../src/memory_byte_store.cpp ../../src/slot_log.cpp ../../src/sd_log.cpp \
	../../src/serial_spool.cpp ../../src/ac_energy_log.cpp ../../src/diag_blackbox.cpp \
	../../src/ac_output_policy.cpp ../../src/ac_output_settings.cpp ../../src/battery_settings.cpp \
	../../src/ac_energy_today.cpp ../../src/stm_energy.cpp ../../src/jbd_bms.cpp ../../src/grid_sense.cpp ../../src/dsp_thermal.cpp ../../src/pzem_v3.cpp
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -Wno-missing-field-initializers
INC := -I$(SRC) -I../../include
# Same feature flags as the product images, for compile-time BMS selection checks.
DSP_FLAGS := -DPOWERCELL_DSP011=1 -DFEATURE_STM32=0 -DFEATURE_LORA=0 -DFEATURE_BAT_SENSE_ADC=0 -DDSP_BMS_UART_SWAP=0
CFGFLAGS := -std=c++17 -Wall -Wextra -Werror -I../../include
all: run
host_tests: test_main.cpp $(wildcard $(SRC)/*.cpp) $(wildcard $(SRC)/*.h) $(CODEC)
	$(CXX) $(CXXFLAGS) $(INC) test_main.cpp $(wildcard $(SRC)/*.cpp) $(CODEC) -o $@
# Compile-only: both DSP ratings select JK-PB 115200 on RX17/TX16; JBD 9600 RX16/TX17 stays buildable; STM stays JK-PB.
check_dsp_bms:
	$(CXX) $(CFGFLAGS) $(DSP_FLAGS) -DDSP_BMS_JKPB=1 -DPOWERCELL_FACTORY_RATED_ACTIVE_W=5000 -DEXPECT_DSP_JKPB test_dsp_bms_config.cpp -o check_dsp_bms_5kw
	$(CXX) $(CFGFLAGS) $(DSP_FLAGS) -DDSP_BMS_JKPB=1 -DPOWERCELL_FACTORY_RATED_ACTIVE_W=3500 -DEXPECT_DSP_JKPB test_dsp_bms_config.cpp -o check_dsp_bms_35kw
	$(CXX) $(CFGFLAGS) $(DSP_FLAGS) -DEXPECT_DSP_JBD test_dsp_bms_config.cpp -o check_dsp_bms_jbd
	$(CXX) $(CFGFLAGS) -DEXPECT_STM_JKPB test_dsp_bms_config.cpp -o check_dsp_bms_stm
	./check_dsp_bms_5kw
	./check_dsp_bms_35kw
	./check_dsp_bms_jbd
	./check_dsp_bms_stm
run: host_tests check_dsp_bms
	./host_tests
clean:
	rm -f host_tests check_dsp_bms_5kw check_dsp_bms_35kw check_dsp_bms_jbd check_dsp_bms_stm
.PHONY: all run clean check_dsp_bms
