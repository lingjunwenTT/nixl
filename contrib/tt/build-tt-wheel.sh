#!/bin/bash
# SPDX-FileCopyrightText: Copyright (c) 2026 Tenstorrent USA, Inc.
# SPDX-License-Identifier: Apache-2.0
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# Builds the Tenstorrent NIXL Python packages. See contrib/tt/README.md.
#
#   contrib/tt/build-tt-wheel.sh
#
# Output, in dist/:
#   nixl_tt-<version>-*.whl   NIXL with Tenstorrent card memory, and the patched UCX inside it
#   nixl-<version>-*.whl      the small "nixl" package that makes `import nixl` load nixl_tt
#
# Environment:
#   PYTHON  Python to build for (default: python3). The wheel only works with that version.
#   WORK    scratch folder for downloads and builds (default: build-tt/ in the repository)
#
# Needs: uv, a C++20 compiler, and the RDMA development headers (libibverbs-dev).

set -euo pipefail

NIXL_DIR=$(cd "$(dirname "$0")/../.." && pwd)
PYTHON=${PYTHON:-python3}
WORK=${WORK:-$NIXL_DIR/build-tt}
DIST=$NIXL_DIR/dist

UCX_VERSION=1.23.0
UCX_URL=https://github.com/openucx/ucx/releases/download/v$UCX_VERSION/ucx-$UCX_VERSION.tar.gz
UCX_PREFIX=$WORK/ucx-install

mkdir -p "$WORK" "$DIST"

echo "=== 1/4 UCX $UCX_VERSION with contrib/tt/ucx-patches ==="
if [ ! -d "$UCX_PREFIX" ]; then
    rm -rf "$WORK/ucx-$UCX_VERSION"
    curl -fsSL "$UCX_URL" | tar xz -C "$WORK"
    cd "$WORK/ucx-$UCX_VERSION"
    for ucx_patch in "$NIXL_DIR"/contrib/tt/ucx-patches/*.patch; do
        patch -p1 < "$ucx_patch"
    done
    # The options NIXL's own wheel build uses (contrib/Dockerfile.manylinux), without CUDA.
    ./contrib/configure-release-mt --prefix="$UCX_PREFIX" \
        --enable-shared --disable-static --disable-doxygen-doc \
        --enable-experimental-api --enable-optimizations --without-avx \
        --enable-cma --enable-devel-headers --with-verbs --with-dm \
        --without-cuda --without-rocm --without-gdrcopy --without-dc \
        --without-rdmacm --without-gga --without-java --without-go --with-ze=no
    make -j"$(nproc)"
    make install
fi

echo "=== 2/4 nixl_tt wheel ==="
VENV=$WORK/venv
uv venv --allow-existing --python "$PYTHON" "$VENV"
uv pip install --python "$VENV/bin/python" \
    meson meson-python pybind11 patchelf pyyaml types-PyYAML tomlkit "setuptools>=80.9.0" \
    auditwheel
export PATH=$VENV/bin:$PATH

# Build from a copy, because the package name is written into pyproject.toml.
SRC=$WORK/src
BUILD=$WORK/nixl-build
rm -rf "$SRC" "$BUILD" "$WORK/wheels"
mkdir -p "$SRC"
git -C "$NIXL_DIR" ls-files -z | (cd "$NIXL_DIR" && xargs -0 cp --parents -t "$SRC")
cd "$SRC"
python contrib/tomlutil.py --wheel-name nixl-tt pyproject.toml
uv build --wheel --no-build-isolation --python "$VENV/bin/python" \
    --out-dir "$WORK/wheels/built" \
    -Cbuild-dir="$BUILD" \
    -Csetup-args=-Ducx_path="$UCX_PREFIX" \
    -Csetup-args=-Dtt_device_memory=true \
    -Csetup-args=-Dwheel_variant=tt \
    -Csetup-args=-Denable_plugins=UCX

echo "=== 3/4 put UCX inside the wheel ==="
# auditwheel copies the libraries the wheel needs into it; LD_LIBRARY_PATH is where it
# finds UCX and the libraries NIXL built from its subprojects.
# The RDMA libraries are left out so that the machine's own are used: they have to
# match the RDMA drivers installed there.
LD_LIBRARY_PATH=$UCX_PREFIX/lib:$BUILD/subprojects/prometheus-cpp auditwheel repair "$WORK"/wheels/built/nixl_tt-*.whl \
    --exclude 'libibverbs*' --exclude 'librdmacm*' --exclude 'libnl*' \
    --wheel-dir "$WORK/wheels/repaired"
python contrib/wheel_add_ucx_plugins.py --ucx-plugins-dir "$UCX_PREFIX/lib/ucx" \
    --skip-nixl-plugins "$WORK"/wheels/repaired/nixl_tt-*.whl
cp "$WORK"/wheels/repaired/nixl_tt-*.whl "$DIST"

echo "=== 4/4 nixl meta wheel ==="
VERSION=$(sed -n 's/^version = "\(.*\)"/\1/p' pyproject.toml)
META=$WORK/meta
rm -rf "$META"
cp -r src/bindings/python/nixl-meta "$META"
cp LICENSE "$META"
sed -e "s/@VERSION@/$VERSION/g" -e "s/@WHEEL_DEPS@/\"nixl-tt==$VERSION\"/" \
    "$META/pyproject.toml.in" > "$META/pyproject.toml"
uv build --wheel --python "$VENV/bin/python" --out-dir "$DIST" "$META"

echo "Built:"
ls "$DIST"/nixl_tt-"$VERSION"-*.whl "$DIST"/nixl-"$VERSION"-*.whl
