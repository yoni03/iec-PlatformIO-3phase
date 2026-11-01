# Unit tests that run on the PC (Linux / macOS):  make -C firmware/test/host
SRC = ../../src
CXXFLAGS = -std=c++17 -O1 -Wall -Wextra -include string -I$(SRC)

run: test_host
	./test_host

test_host: test_main.cpp $(SRC)/p1_parser.cpp $(SRC)/records.cpp $(SRC)/tariff.cpp $(SRC)/demo.cpp
	$(CXX) $(CXXFLAGS) -o $@ $^

clean:
	rm -f test_host simulate

.PHONY: run clean

# End-to-end simulation (also produces data for tools/mock_server.py)
simulate: simulate.cpp $(SRC)/p1_parser.cpp $(SRC)/records.cpp $(SRC)/tariff.cpp $(SRC)/demo.cpp
	$(CXX) $(CXXFLAGS) -O2 -o $@ $^

sim-data: simulate
	mkdir -p ../../tools/sim-data && ./simulate ../../tools/sim-data 400
