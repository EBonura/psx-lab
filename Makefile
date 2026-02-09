# psx-lab - PS1 Development Playground
# Uses PSYQo (C++20) via nugget submodule

PROJECTS = room_test celeste

.PHONY: all clean $(PROJECTS)

all: $(PROJECTS)

$(PROJECTS):
	@echo "Building $@..."
	@$(MAKE) -C src/$@ BUILD=$(BUILD)

clean:
	@for p in $(PROJECTS); do $(MAKE) -C src/$$p clean; done
	@rm -rf release/*
