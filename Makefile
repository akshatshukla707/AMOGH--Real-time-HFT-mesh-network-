# ================================================================
#  MAKEFILE — SCP-L2 Demo
#  Synchronized Chaff Protocol over Layer 2
# ================================================================
#
#  BUILD:   make
#  CLEAN:   make clean
#  RUN:     see README.txt
# ================================================================

CXX      = g++
CXXFLAGS = -std=c++17 -O2 -pthread -Wall -Wextra

# What we build
TARGETS = sender receiver

all: $(TARGETS)
	@echo ""
	@echo "Build complete."
	@echo "Run: ./receiver           (in terminal 1)"
	@echo "Run: ./sender 127.0.0.1 1 (in terminal 2)"
	@echo ""

sender: scp_sender.cpp scp_common.h
	$(CXX) $(CXXFLAGS) -o sender scp_sender.cpp
	@echo "Built: sender"

receiver: scp_receiver.cpp scp_common.h
	$(CXX) $(CXXFLAGS) -o receiver scp_receiver.cpp
	@echo "Built: receiver"

clean:
	rm -f sender receiver

.PHONY: all clean
