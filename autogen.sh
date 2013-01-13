#!/bin/sh

set -e
autoreconf --install --verbose
./configure "$@"
