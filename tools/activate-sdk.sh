#!/usr/bin/env sh
if [ -z "${CONDA_PREFIX:-}" ] || [ ! -f "$CONDA_PREFIX/dev-env.sh" ]; then
    printf '%s\n' 'The orocos-dev development environment is required.' >&2
    return 1
fi
. "$CONDA_PREFIX/dev-env.sh"
# The released SDK supplies third-party dependencies. Port headers, libraries
# and plugins must all come from the matching RTT 3 build.
rtt_http_prefix="${PIXI_PROJECT_ROOT:-$PWD}/.ci-dependencies/install"
export CMAKE_PREFIX_PATH="$rtt_http_prefix:${CMAKE_PREFIX_PATH:-}"
export PKG_CONFIG_PATH="$rtt_http_prefix/lib/pkgconfig:${PKG_CONFIG_PATH:-}"
export LD_LIBRARY_PATH="$rtt_http_prefix/lib:$rtt_http_prefix/lib/orocos/gnulinux/types:$rtt_http_prefix/lib/orocos/gnulinux/plugins:${LD_LIBRARY_PATH:-}"
export PATH="$rtt_http_prefix/bin:$PATH"
export RTT_COMPONENT_PATH="$rtt_http_prefix/lib/orocos"
export OROCOS_COMPONENT_PATH="$RTT_COMPONENT_PATH"
