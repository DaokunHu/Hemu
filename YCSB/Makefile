CXX = g++

CXXFLAGS = -std=c++17 -Wall -Wextra -O3
TARGETS = gen_workload print_workload

all: $(TARGETS)

gen_workload: gen_workload.cpp
	$(CXX) $(CXXFLAGS) $< -o $@

print_workload: print_workload.cpp
	$(CXX) $(CXXFLAGS) $< -o $@

clean:
	rm -f $(TARGETS)
