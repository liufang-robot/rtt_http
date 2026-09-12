#!/usr/bin/env bash
set -euo pipefail
repository="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
source "$repository/tools/activate-sdk.sh"
staging="$repository/.ci-dependencies/install"
build="$repository/.ci-dependencies/build-rtt"
cmake -S "$repository/.ci-dependencies/rtt" -B "$build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$staging" \
    -DOROCOS_TARGET=gnulinux -DENABLE_CORBA=OFF -DENABLE_TESTS=OFF \
    -DBUILD_TESTING=OFF -DBUILD_DOCS=OFF -DPLUGINS_ENABLE=ON \
    -DPLUGINS_ENABLE_TYPEKIT=ON -DPLUGINS_ENABLE_SCRIPTING=ON \
    -DPLUGINS_ENABLE_MARSHALLING=ON -DDEFAULT_PLUGIN_PATH="$staging/lib/orocos"
cmake --build "$build" --parallel "${CMAKE_BUILD_PARALLEL_LEVEL:-2}"
cmake --install "$build"
