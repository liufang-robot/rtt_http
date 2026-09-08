#!/usr/bin/env sh
if [ -z "${CONDA_PREFIX:-}" ] || [ ! -f "$CONDA_PREFIX/dev-env.sh" ]; then
    printf '%s\n' 'The orocos-dev development environment is required.' >&2
    return 1
fi
. "$CONDA_PREFIX/dev-env.sh"
