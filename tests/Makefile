# Host tests for the daikin_diii component.
#   make -C tests test
# - units:   write guards, queue/retry timing and hand-checked decoder values
# - isp:     the ATmega programmer (STK500v1 -> ISP) against a simulated ATmega328P
# - decoder: C++ decoder vs the Python reference (tools/f1f2_decode.py) over every
#            bus log in captures/; skipped when there are none (see captures/README.md)
CXX ?= c++
CXXFLAGS ?= -std=c++17 -O1 -Wall -Wextra -Werror
INC = -I../components/daikin_diii
HDRS = ../components/daikin_diii/diii_decoder.h ../components/daikin_diii/diii_writer.h
CAPTURES = $(wildcard ../captures/*.tsv)
BUILD ?= build

.PHONY: test units isp decoder clean

test: units isp decoder

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/test_units: test_units.cpp $(HDRS) | $(BUILD)
	$(CXX) $(CXXFLAGS) $(INC) $< -o $@

$(BUILD)/test_isp: test_isp.cpp ../components/daikin_diii/avr_isp.h | $(BUILD)
	$(CXX) $(CXXFLAGS) $(INC) $< -o $@

$(BUILD)/test_decoder: test_decoder.cpp $(HDRS) | $(BUILD)
	$(CXX) $(CXXFLAGS) $(INC) $< -o $@

units: $(BUILD)/test_units
	$(BUILD)/test_units

isp: $(BUILD)/test_isp
	$(BUILD)/test_isp

ifeq ($(strip $(CAPTURES)),)
decoder:
	@echo "decoder: skipped, no bus logs in captures/"
else
decoder: $(BUILD)/test_decoder
	$(BUILD)/test_decoder $(CAPTURES) > $(BUILD)/cpp.txt
	python3 reference.py $(CAPTURES) > $(BUILD)/py.txt
	diff -u $(BUILD)/py.txt $(BUILD)/cpp.txt
	@echo "decoder: C++ == Python on $(words $(CAPTURES)) captures"
endif

clean:
	rm -rf $(BUILD)
