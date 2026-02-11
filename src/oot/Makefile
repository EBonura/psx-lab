TARGET = oot
TYPE = ps-exe

SRCS = main.cpp room.cpp skeleton.cpp

LDFLAGS += -lgcc

include ../../third_party/nugget/psyqo-paths/psyqo-paths.mk

.PHONY: deploy
deploy: all
	@mkdir -p ../../release
	@cp $(TARGET).ps-exe ../../release/$(TARGET).ps-exe
	@echo "Deployed to ../../release/$(TARGET).ps-exe"

.PHONY: disc
disc: deploy
	mkpsxiso -y ../../tools/oot/cd.xml -o ../../release/oot.bin -c ../../release/oot.cue
	@echo "CD image: ../../release/oot.bin / oot.cue"

.DEFAULT_GOAL := deploy
