#!/bin/sh
# host/gl12 的本地测试（macOS / Linux，clang）：不需要 Windows，也不需要设备。
#   shim/          极简 windows.h 替身
#   fake_espgpu    espgpu.dll 替身：命令体格式与 host/src 一致，按 24 KB chunk + LZ 计下行字节，
#                  命令流交给 firmware/core 真正渲染
#   sem_test       GL 语义测试（平面着色激发顶点、纹理解包、显示列表、读像素、WGL 线程规则、掉线重连……）
#   doom_frame     仿 prboom-plus 调用流的一帧，打印每帧下行字节 / 命令数 / 裂缝像素
#                  （选项：--lists --irr --sub N --jitter --nofog --frames N --bmp out.bmp --norender）
#
#   sh host/gl12/tests/run.sh [输出目录]
set -e
H=$(cd "$(dirname "$0")" && pwd)
R=$(cd "$H/../../.." && pwd)
SRC=$R/host/gl12
OUT=${1:-$H/out}
mkdir -p "$OUT/core"
for f in "$R"/firmware/core/*.c; do
  clang -std=c11 -O2 -g -I"$R/shared" -I"$R/firmware/core" -c "$f" -o "$OUT/core/$(basename "$f" .c).o"
done
GL="$SRC/gl_state.cpp $SRC/gl_matrix.cpp $SRC/gl_vertex.cpp $SRC/gl_draw.cpp $SRC/gl_texture.cpp $SRC/gl_list.cpp \
    $SRC/gl_misc.cpp $SRC/gl_device.cpp $SRC/gl_trace.cpp $SRC/gl_wgl.cpp $SRC/gen/gl_exports.cpp $SRC/gen/gl_stubs.cpp"
FLAGS="-std=c++20 -g -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -DESPGPU_STATIC \
       -I$H/shim -I$SRC -I$R/host/include -I$R/shared -I$R/firmware/core"
clang++ $FLAGS -O1 -fsanitize=address,undefined $GL "$H/fake_espgpu.cpp" "$H/sem_test.cpp" "$OUT"/core/*.o -o "$OUT/sem_test"
clang++ $FLAGS -O2 $GL "$H/fake_espgpu.cpp" "$H/doom_frame.cpp" "$OUT"/core/*.o -o "$OUT/doom_frame"
ASAN_OPTIONS=detect_leaks=0 "$OUT/sem_test"
"$OUT/doom_frame"
"$OUT/doom_frame" --irr --jitter
"$OUT/doom_frame" --irr --sub 2 --jitter --lists
