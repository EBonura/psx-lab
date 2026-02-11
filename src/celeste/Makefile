TARGET = celeste
TYPE = ps-exe

SRCS = main.cpp celeste.cpp compat.cpp psx_audio.cpp

CPPFLAGS += -isystem libc
LDFLAGS += -lgcc
DEFINES = CELESTE_P8_FIXEDP

include ../../third_party/nugget/psyqo/psyqo.mk

.PHONY: deploy
deploy: all
	@mkdir -p ../../release
	@cp $(TARGET).ps-exe ../../release/$(TARGET).ps-exe
	@echo "Deployed to ../../release/$(TARGET).ps-exe"

.PHONY: disc
disc: deploy
	mkpsxiso -y ../../tools/celeste/cd.xml -o ../../release/celeste.bin -c ../../release/celeste.cue
	@echo "CD image: ../../release/celeste.bin / celeste.cue"

.DEFAULT_GOAL := deploy
