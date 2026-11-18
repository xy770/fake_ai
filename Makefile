# ∞ 无限额度 · 假AI —— 单文件 C99，零第三方依赖
# Arch: sudo pacman -S gcc make   （只需 base-devel）
CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -Wno-unused-parameter -std=c99 -D_GNU_SOURCE
LDFLAGS ?= -pthread
BIN      = fakeai

all: $(BIN)

# frontend.h 已内联好前端，正常不需要重新生成。
# 只有你改了 chat.html / admin.html 才跑： make frontend
frontend:
	python3 gen_frontend.py

$(BIN): fakeai.c frontend.h
	$(CC) $(CFLAGS) -o $@ fakeai.c $(LDFLAGS)

clean:
	rm -f $(BIN)

# 上课前本机试跑： make run
run: $(BIN)
	FAKEAI_TOKEN=class2026 ./$(BIN) 127.0.0.1 9000

.PHONY: all clean run frontend
