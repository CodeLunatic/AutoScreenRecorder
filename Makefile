# AutoScreenRecorder — Windows + CGO (g++/w64devkit 或 MSYS2)
# 用法: make          编译
#       make run      编译并运行
#       make clean    删除 exe

BINARY   := AutoScreenRecorder.exe
CMD      := ./cmd/autorecorder
CONFIG   := config.yaml
GOBUILD  := go build -trimpath -ldflags "-s -w -H windowsgui" -o $(BINARY) $(CMD)

export CGO_ENABLED := 1
export CXX         := g++

.PHONY: all build run clean config help

all: build

help:
	@echo Targets:
	@echo   make        编译 $(BINARY)
	@echo   make run    编译并运行（需 $(CONFIG)）
	@echo   make config 从 config.example.balanced.yaml 复制配置（若不存在）
	@echo   make clean  删除 $(BINARY)

config:
	@if [ ! -f "$(CONFIG)" ]; then \
		cp config.example.balanced.yaml "$(CONFIG)" && echo "created $(CONFIG)"; \
	else \
		echo "$(CONFIG) already exists"; \
	fi

# BUILD_STAMP 每次不同，Go 会因此重新编译 C++，其它纯 Go 包仍走缓存。
build: config
	CGO_CXXFLAGS="$(CGO_CXXFLAGS) -DBUILD_STAMP=$$(date +%s)" $(GOBUILD)
	@echo "built: $(BINARY)"

run: build
	./$(BINARY) -config "$(CONFIG)"

clean:
	@rm -f "$(BINARY)"
	@echo "removed $(BINARY)"
