TARGET = oot
TYPE = ps-exe

SRCS = main.cpp

LDFLAGS += -lgcc

include ../../third_party/nugget/psyqo/psyqo.mk

.PHONY: deploy
deploy: all
	@mkdir -p ../../release
	@cp $(TARGET).ps-exe ../../release/$(TARGET).ps-exe
	@echo "Deployed to ../../release/$(TARGET).ps-exe"

.DEFAULT_GOAL := deploy
