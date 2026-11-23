CXX := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -pedantic -O2 -Iinclude

TARGET := scheduler_sim
SOURCES := main.cpp src/scheduler_sim.cpp src/schedulers.cpp

.PHONY: all clean run

all: $(TARGET)

$(TARGET): $(SOURCES) include/scheduler_sim.h
	$(CXX) $(CXXFLAGS) $(SOURCES) -o $(TARGET)

run: $(TARGET)
	./$(TARGET) | tee scheduler_results.csv

clean:
	rm -f $(TARGET)
