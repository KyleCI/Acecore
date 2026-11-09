SRC := ../../lib/pc_core/src
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -Wno-missing-field-initializers
all: run
host_tests: test_main.cpp $(wildcard $(SRC)/*.cpp) $(wildcard $(SRC)/*.h)
	$(CXX) $(CXXFLAGS) -I$(SRC) test_main.cpp $(wildcard $(SRC)/*.cpp) -o $@
run: host_tests
	./host_tests
clean:
	rm -f host_tests
.PHONY: all run clean
