TARGET = room_test
TYPE = ps-exe

SRCS = room_test.cpp

CXXFLAGS = -std=c++20

include ../../third_party/nugget/psyqo/psyqo.mk

.PHONY: deploy
deploy: all
	@mkdir -p ../../release
	@cp $(TARGET).ps-exe ../../release/$(TARGET).ps-exe
	@echo "Deployed to ../../release/$(TARGET).ps-exe"

.DEFAULT_GOAL := deploy
