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

.DEFAULT_GOAL := deploy
