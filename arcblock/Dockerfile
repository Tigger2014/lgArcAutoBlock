# arcblock builder — plain Docker Desktop.
# TV userspace is SOFT-float ARMv7-A (eim e_flags 0x5000200; hard-float binaries
# die in the TV loader with "internal error"). armel + softfp matches the TV:
# soft calling convention, VFP codegen (TV has VFPv3+NEON).
# Every artifact is build-time checked for flags 0x5000200 (TV-ABI contract).
# Gum-free: hooks are Thumb-2 inline patches (hookthumb.c), proven functionally
# by the qemu-arm test below (fails the build on any wrong encoding).
FROM debian:bookworm-slim

ARG DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
      gcc-arm-linux-gnueabi g++-arm-linux-gnueabi libc6-dev-armel-cross \
      gcc g++ \
      cmake make git ca-certificates \
      qemu-user-static \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /w

# ezinject (canonical repo), soft-float for the TV.
RUN git clone --depth 1 https://github.com/smx-smx/ezinject.git /w/ezinject
COPY toolchain-armel.cmake /w/toolchain-armel.cmake
RUN cmake -S /w/ezinject -B /w/ezinject/build \
      -DCMAKE_TOOLCHAIN_FILE=/w/toolchain-armel.cmake \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_EXE_LINKER_FLAGS="-Wl,--dynamic-linker,/lib/ld-linux.so.3" \
 && cmake --build /w/ezinject/build -j"$(nproc)"

# Check helper: every artifact must carry TV-native flags 0x5000200.
COPY check-abi.sh /w/check-abi.sh

# qemu-arm functional test of the hook engine: literal/BL/B/ADR rewrites plus
# the loud-refuse path. Runs the REAL engine + gas-assembled mocks on ARM.
COPY arcblock.cpp hookthumb.h hookthumb.c test_hook.c test_mock.S /w/
RUN arm-linux-gnueabi-gcc -mfloat-abi=softfp -march=armv7-a -mfpu=vfpv3 \
      -Os -c -o /tmp/test_hook.o /w/test_hook.c -I/w && \
    arm-linux-gnueabi-gcc -mfloat-abi=softfp -march=armv7-a -mfpu=vfpv3 \
      -Os -c -o /tmp/test_mock.o /w/test_mock.S && \
    arm-linux-gnueabi-gcc -mfloat-abi=softfp -march=armv7-a -mfpu=vfpv3 \
      -Os -c -o /tmp/hookthumb.o /w/hookthumb.c -I/w && \
    arm-linux-gnueabi-g++ -mfloat-abi=softfp -march=armv7-a -mfpu=vfpv3 \
      -Os -c -o /tmp/arcblock_pred.o /w/arcblock.cpp -I/w && \
    arm-linux-gnueabi-g++ -mfloat-abi=softfp -march=armv7-a -mfpu=vfpv3 \
      -Os -o /w/test_hook /tmp/test_hook.o /tmp/test_mock.o /tmp/hookthumb.o /tmp/arcblock_pred.o -ldl -lpthread && \
    qemu-arm-static -L /usr/arm-linux-gnueabi /w/test_hook

# arcblock.so — our Gum-free injector: arcblock.cpp + hookthumb.c + ezinject CRT.
# Minimal link (libdl for dlsym, libpthread for the worker thread).
RUN mkdir -p /w/out && \
    CRT_OBJS="$(find /w/ezinject/build -path '*ezinject_crt.dir*' -name '*.o')" && \
    UTIL_A="$(find /w/ezinject/build -name 'libezinject_util.a' | head -1)" && \
    LOG_A="$(find /w/ezinject/build -name 'libezinject_log.a' | head -1)" && \
    arm-linux-gnueabi-g++ -mfloat-abi=softfp -march=armv7-a -mfpu=vfpv3 \
      -ffunction-sections -fdata-sections -shared -fPIC -Os \
      -o /w/out/arcblock.so /w/arcblock.cpp /w/hookthumb.c \
      $CRT_OBJS $UTIL_A $LOG_A \
      -I/w/ezinject -I/w/ezinject/build -I/w/ezinject/crt/lh \
      -ldl -lpthread \
      -Wl,--gc-sections -Wl,-z,noexecstack && \
    /w/check-abi.sh /w/out/arcblock.so && \
    arm-linux-gnueabi-nm -D /w/out/arcblock.so | grep -aE 'crt_init|lib_main' && \
    ls -lh /w/out/

# copy the built ezinject ARM binary next to the modules
RUN BIN="$(find /w/ezinject/build -maxdepth 3 -name ezinject -type f | head -1)" && \
    test -n "$BIN" && cp "$BIN" /w/out/ezinject && \
    /w/check-abi.sh /w/out/ezinject && ls -lh /w/out/
