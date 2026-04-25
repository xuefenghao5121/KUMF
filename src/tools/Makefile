# KUMF SPE Self-Profiling Tools
# 编译不需要 sudo，运行也不需要 sudo

CC ?= gcc
CFLAGS = -Wall -O2 -g -D_GNU_SOURCE
LDFLAGS = -lpthread

all: spe_self_profile spe_preload.so

spe_self_profile: spe_self_profile.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

spe_preload.so: spe_preload.c spe_self_profile.c
	$(CC) $(CFLAGS) -shared -fPIC -o $@ spe_preload.c spe_self_profile.c $(LDFLAGS)

clean:
	rm -f spe_self_profile spe_preload.so

# 测试: 不需要 sudo
test: all
	@echo "=== 测试 SPE self-profiling ==="
	@mkdir -p /tmp/kumf
	LD_PRELOAD=./spe_preload.so sleep 2
	@echo "=== 检查输出 ==="
	@ls -la /tmp/kumf/spe_preload_*.txt 2>/dev/null || echo "No output yet"
	@echo "=== 测试 wrapper 模式 ==="
	./spe_self_profile -- sleep 2
	@ls -la /tmp/kumf/spe_self_profile.txt 2>/dev/null || echo "No output yet"
