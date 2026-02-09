# psx-lab - PS1 Development Playground
# Uses PSYQo (C++20) via nugget submodule

.PHONY: all clean

all:
	@$(MAKE) -C src BUILD=$(BUILD)

clean:
	@$(MAKE) -C src clean
	@rm -rf release/*
