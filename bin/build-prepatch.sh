#!/usr/bin/env bash
set -e

WD="$(pwd)/$(dirname $0)"


pushd "${WD}/../envoy"
git apply "${WD}/build.patch"
popd