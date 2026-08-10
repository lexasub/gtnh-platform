#!/usr/bin/env bash
# make-offline-bundle.sh — собирает оффлайн-бандл зависимостей для gtnh-platform.
#
# Запускать на машине разработчика (где есть Conan-кэш и собраны bgfx/bx/bimg в /usr/local).
# Результат: cmake-build-offline/gtnh-platform-deps-<tag>.tar.xz — готов к выкладке
# в GitHub Release. Потребитель НЕ устанавливает Conan: распаковал → ./install-deps.sh
# → cmake с toolchain из бандла → ninja.
#
# Шаги перед запуском (один раз, нужна сеть):
#   conan install -of cmake-build-offline/gen --build=missing
# (свежие конфиги генерируются из conanfile.txt; пакеты берутся из ~/.conan2/p)
#
# Структура бандла:
#   deps/p/<short>/...           пакеты из ~/.conan2/p (заголовки, .a, flatc)
#   deps/conan-offline/*.cmake   Conan-конфиги с относительными путями
#   deps/usr-local/              bgfx/bx/bimg (ставится в /usr/local)
#   install-deps.sh              установка bgfx/bx/bimg в /usr/local
#   README-OFFLINE.md            инструкция для потребителя
#
# Известная проблема: Release-сборка на GCC 15 падает на -Werror
# (unused-but-set-variable в MutableSection.cpp, unused-parameter в TextureAtlas.cpp).
# Багфикс кода в трекере; для обхода: -DCMAKE_CXX_FLAGS="-Wno-error" (см. README-OFFLINE.md).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="${1:-$(dirname "$SCRIPT_DIR")}"            # корень репо (по умолчанию — родитель scripts/)
GEN_DIR="${2:-$REPO_DIR/cmake-build-offline/gen}"    # результаты `conan install -of ...`
OUT_DIR="${3:-$REPO_DIR/cmake-build-offline/bundle}" # сборка бандла
TAG="${4:-$(date +%F)}"

BUNDLE_DEPS="$OUT_DIR/deps"

echo "==> Чистим $OUT_DIR"
rm -rf "$OUT_DIR"
mkdir -p "$BUNDLE_DEPS/p" "$BUNDLE_DEPS/conan-offline" "$BUNDLE_DEPS/usr-local/include" "$BUNDLE_DEPS/usr-local/lib"

if ! compgen -G "$GEN_DIR/*.cmake" > /dev/null; then
    echo "ОШИБКА: в $GEN_DIR нет .cmake — сначала: conan install -of $GEN_DIR --build=missing" >&2
    exit 1
fi

echo "==> Копируем Conan-пакеты (по путям из конфигов)"
PKG_DIRS=$(grep -hoE '/home/[^/]+/\.conan2/p(/b)?/[a-z0-9._-]+' "$GEN_DIR"/*.cmake | sort -u)
for d in $PKG_DIRS; do
    rel="${d#*/.conan2/p/}"
    if [ -d "$d/p" ]; then
        mkdir -p "$BUNDLE_DEPS/p/$rel/p"
        cp -r "$d/p/." "$BUNDLE_DEPS/p/$rel/p/"
        echo "  + p/$rel ($(du -sh "$BUNDLE_DEPS/p/$rel/p" | cut -f1))"
    else
        echo "  ! пропущен (нет p/): $d"
    fi
done
N_A=$(find "$BUNDLE_DEPS/p" -name '*.a' | wc -l)
N_FB=$(find "$BUNDLE_DEPS/p" -name flatc | wc -l)
echo "  итого: $N_A статических либ, flatc: $([ "$N_FB" -gt 0 ] && echo есть || echo НЕТ!)"

echo "==> Копируем bgfx/bx/bimg из /usr/local"
for inc in bgfx bimg bx; do
    [ -d "/usr/local/include/$inc" ] && cp -r "/usr/local/include/$inc" "$BUNDLE_DEPS/usr-local/include/"
done
mkdir -p "$BUNDLE_DEPS/usr-local/lib"
cp /usr/local/lib/libbgfx.a /usr/local/lib/libbimg*.a /usr/local/lib/libbx.a "$BUNDLE_DEPS/usr-local/lib/" 2>/dev/null || true
echo "  + include: $(ls "$BUNDLE_DEPS/usr-local/include" | tr '\n' ' ')"
echo "  + lib:     $(ls "$BUNDLE_DEPS/usr-local/lib" | tr '\n' ' ')"

echo "==> Конфиги с относительными путями (sed /home/.../.conan2/p -> \${CMAKE_CURRENT_LIST_DIR}/../p)"
cp "$GEN_DIR"/*.cmake "$BUNDLE_DEPS/conan-offline/"
sed -i 's|/home/[^/]*/\.conan2/p/|${CMAKE_CURRENT_LIST_DIR}/../p/|g' "$BUNDLE_DEPS/conan-offline"/*.cmake
echo "  + $(ls "$BUNDLE_DEPS/conan-offline"/*.cmake | wc -l) cmake-файлов"

echo "==> install-deps.sh"
cat > "$OUT_DIR/install-deps.sh" <<'EOF'
#!/usr/bin/env bash
# Ставит bgfx/bx/bimg из бандла в /usr/local (нужен sudo). Остальное — без прав.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
echo "==> bgfx/bx/bimg -> /usr/local"
# Копируем ВСЁ содержимое include/ и lib/ — работает и с плоскими
# заголовками (bgfx.h), и с вложенными папками (bgfx/, bimg/, bx/).
sudo cp -r "$HERE/deps/usr-local/include/." /usr/local/include/
sudo cp "$HERE/deps/usr-local/lib/"*.a /usr/local/lib/
echo "Готово. Проверь: ls /usr/local/lib/libbgfx.a /usr/local/include/bgfx"
EOF
chmod +x "$OUT_DIR/install-deps.sh"

echo "==> README-OFFLINE.md"
cat > "$OUT_DIR/README-OFFLINE.md" <<'EOF'
# Оффлайн-сборка gtnh-platform (без Conan)

В этом архиве — все зависимости, кроме системных. Conan не нужен и не запускается.

## Шаг 1. Системные пакеты

Arch:   sudo pacman -S base-devel cmake ninja go tbb
Ubuntu: sudo apt install build-essential cmake ninja-build golang libtbb-dev

Также нужны X11/GL dev-пакеты (libx11-dev, libgl1-mesa-dev, libxcb-*, ...) — стандартный набор для графики.

## Шаг 2. bgfx/bx/bimg (один раз)

./install-deps.sh    # спросит sudo

## Шаг 3. Сборка

# исходники — рядом (репозиторий или архив релиза)
cmake -GNinja -DCMAKE_TOOLCHAIN_FILE=$PWD/deps/conan-offline/conan_toolchain.cmake -S repo -B build
ninja -C build -j$(nproc)

## Если сборка падает на -Werror (GCC 15)

Известные мёртвые предупреждения в Release (багфикс в трекере). Обход:

cmake -GNinja -DCMAKE_TOOLCHAIN_FILE=$PWD/deps/conan-offline/conan_toolchain.cmake \
      -DCMAKE_CXX_FLAGS="-Wno-error" -S repo -B build

## Системные зависимости (не входят в бандл)

- TBB (libtbb-dev)        — /usr/lib/cmake/TBB
- X11/GL-стек             — libx11-dev, libgl1-mesa-dev, xorg
- Go 1.22+                — для routerd и metadbd
- Компилятор C++26        — GCC 14+ или Clang 18+

Всё остальное (asio, entt, glm, flatbuffers+flatc, spdlog, fmt, glfw3, imgui,
lodepng, yaml-cpp, liburing, lmdb, sqlite3, fastnoise2, nlohmann_json, miniaudio)
лежит в deps/p/ — без Conan, без сети.
EOF

echo "==> Пакую: cmake-build-offline/gtnh-platform-deps-$TAG.tar.xz"
tar -cJf "$REPO_DIR/cmake-build-offline/gtnh-platform-deps-$TAG.tar.xz" -C "$OUT_DIR" .
ls -lh "$REPO_DIR/cmake-build-offline/gtnh-platform-deps-$TAG.tar.xz"
echo "Бандл готов. Для релиза: gh release upload <tag> cmake-build-offline/gtnh-platform-deps-$TAG.tar.xz"
