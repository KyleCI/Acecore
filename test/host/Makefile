SRC := ../../lib/pc_core/src
CODEC := ../../src/ds3231_time.cpp ../../src/lagos_time.cpp ../../src/battery_alert_policy.cpp \
	../../src/memory_byte_store.cpp ../../src/slot_log.cpp ../../src/sd_log.cpp \
	../../src/serial_spool.cpp ../../src/ac_energy_log.cpp ../../src/diag_blackbox.cpp \
	../../src/ac_output_policy.cpp ../../src/ac_output_settings.cpp ../../src/battery_settings.cpp \
	../../src/ac_energy_today.cpp
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -Wno-missing-field-initializers
all: run
host_tests: test_main.cpp $(wildcard $(SRC)/*.cpp) $(wildcard $(SRC)/*.h) $(CODEC)
	$(CXX) $(CXXFLAGS) -I$(SRC) -I../../include test_main.cpp $(wildcard $(SRC)/*.cpp) $(CODEC) -o $@
run: host_tests
	./host_tests
clean:
	rm -f host_tests
.PHONY: all run clean
