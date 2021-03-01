#!/usr/bin/env bash
set -e

WD="$(pwd)/$(dirname $0)"


pushd "${WD}/../envoy"
git apply "${WD}/build.patch"
popd

pushd "${WD}/../ModSecurity"
git apply "${WD}/modsec-see-errors.patch"
popd