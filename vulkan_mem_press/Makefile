CXX = g++
CXXFLAGS = -std=c++17 -O2
LDFLAGS = -lvulkan

TARGETS = vk_memstress vk_compute_min

all: $(TARGETS)

vk_memstress: vk_memstress.cpp
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

vk_compute_min: vk_compute_min.cpp
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

clean:
	rm -f $(TARGETS)
