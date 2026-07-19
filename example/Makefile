LOP_HDL ?= python3 main.py
TB_CPP ?= tb.cpp
GEN_TB_H ?= sh gen-tb-h.sh
TEST ?= test.py
CLK ?= 

tb: $(TB_CPP) TopSim.v
	$(VERILATOR) --trace-underscore --no-timing --x-initial-edge --cc --trace -Wno-lint --exe --build -O3 --x-assign fast --x-initial fast --noassert -Wno-fatal --top $(TOP) \
		-CFLAGS "-I$(PWD)" \
		--build-jobs $(shell nproc) \
		$^

run:
	PYTHONPATH=$(PWD) python3 test.py obj_dir/VTopSim

TopSim.v: king.lop
	$(LOP_HDL) gen-verilog $(TOP) $^ > TopSim.v
	$(GEN_TB_H) $(TOP) $^ $(CLK)

clean:
	rm -fr __pycache__
	rm -fr obj_dir/
	rm -f tb-sig.h tb-top.h tb.py
	rm -f TopSim.v
	rm -f simx.vcd
