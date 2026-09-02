#!/bin/sh
# SPDX-License-Identifier: MIT

# Install the pinned Piglit/Waffle development harness beside Mesa.

set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
mesa_root=$(CDPATH= cd -- "$script_dir/../../../.." && pwd)
workspace=$(dirname "$mesa_root")

piglit_root=${PIGLIT_ROOT:-$workspace/piglit}
piglit_build=${PIGLIT_BUILD:-$workspace/piglit-build}
piglit_local=${PIGLIT_LOCAL:-$workspace/piglit-local}
waffle_root=${WAFFLE_ROOT:-$workspace/waffle}
waffle_build=${WAFFLE_BUILD:-$workspace/waffle-build}

piglit_revision=372590d39085e8640742341b5ec5a8a72ad1c56f
waffle_tag=v1.8.3

if [ ! -d "$waffle_root/.git" ]; then
    git clone --branch "$waffle_tag" --depth 1 \
        https://gitlab.freedesktop.org/mesa/waffle.git "$waffle_root"
fi

if [ "$(git -C "$waffle_root" describe --tags --exact-match 2>/dev/null || true)" != "$waffle_tag" ]; then
    echo "$waffle_root is not checked out at $waffle_tag" >&2
    exit 1
fi

if [ ! -d "$piglit_root/.git" ]; then
    git clone --no-checkout \
        https://gitlab.freedesktop.org/mesa/piglit.git "$piglit_root"
    git -C "$piglit_root" checkout --detach "$piglit_revision"
fi

if [ "$(git -C "$piglit_root" rev-parse HEAD)" != "$piglit_revision" ]; then
    echo "$piglit_root is not checked out at $piglit_revision" >&2
    exit 1
fi

if [ -f "$waffle_build/meson-private/coredata.dat" ]; then
    meson_action=--reconfigure
else
    meson_action=
fi

meson setup $meson_action "$waffle_build" "$waffle_root" \
    --prefix="$piglit_local" \
    --libdir=lib \
    -Dglx=disabled \
    -Dwayland=disabled \
    -Dx11_egl=disabled \
    -Dgbm=disabled \
    -Dsurfaceless_egl=enabled \
    -Dbuild-tests=false \
    -Dbuild-examples=false
meson compile -C "$waffle_build"
meson install -C "$waffle_build"

if [ ! -x "$piglit_local/venv/bin/python" ]; then
    uv venv --python 3.14 "$piglit_local/venv"
fi
uv pip install --python "$piglit_local/venv/bin/python" \
    'numpy>=1.13' 'mako>=1.0.2'

PKG_CONFIG_PATH="$piglit_local/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}" \
LD_LIBRARY_PATH="$piglit_local/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
cmake -S "$piglit_root" -B "$piglit_build" -G Ninja \
    -DPYTHON_EXECUTABLE="$piglit_local/venv/bin/python" \
    -DPIGLIT_USE_WAFFLE=ON \
    -DPIGLIT_USE_GBM=OFF \
    -DPIGLIT_USE_WAYLAND=OFF \
    -DPIGLIT_USE_X11=OFF \
    -DPIGLIT_BUILD_GL_TESTS=ON \
    -DPIGLIT_BUILD_GLX_TESTS=OFF \
    -DPIGLIT_BUILD_EGL_TESTS=ON \
    -DPIGLIT_BUILD_GLES1_TESTS=OFF \
    -DPIGLIT_BUILD_GLES2_TESTS=OFF \
    -DPIGLIT_BUILD_GLES3_TESTS=ON \
    -DPIGLIT_BUILD_VK_TESTS=OFF \
    -DPIGLIT_BUILD_DMA_BUF_TESTS=OFF
cmake --build "$piglit_build" \
    --target shader_runner shader_runner_gles3 --parallel

# Piglit only imports Python profiles from its tests package. Keep the profiles
# owned by Mesa and expose them through ignored development-only symlinks.
exclude=$piglit_root/.git/info/exclude
for profile in apple9_compute apple9_desktop_compute; do
    profile_link=$piglit_root/tests/$profile.py
    if [ -e "$profile_link" ] && [ ! -L "$profile_link" ]; then
        echo "$profile_link exists and is not the expected symlink" >&2
        exit 1
    fi
    ln -sfn "$script_dir/$profile.py" "$profile_link"

    exclude_entry=/tests/$profile.py
    if ! grep -qxF "$exclude_entry" "$exclude"; then
        printf '%s\n' "$exclude_entry" >>"$exclude"
    fi
done

echo "T8132_PIGLIT_SETUP_OK"
echo "piglit=$piglit_revision"
echo "waffle=$waffle_tag"
echo "desktop_runner=$piglit_build/bin/shader_runner"
echo "gles3_runner=$piglit_build/bin/shader_runner_gles3"
