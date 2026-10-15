CXX = g++-12
CXXFLAGS = -std=c++2a -O3
LDFLAGS = -pthread -lnuma -lpmem

Hemu: main.cpp
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDFLAGS)

clean:
	rm -f Hemu

.PHONY: clean
