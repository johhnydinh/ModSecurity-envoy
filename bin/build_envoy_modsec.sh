#!/usr/bin/env bash
set -e

WD="$(pwd)/$(dirname $0)"

result_dir="${WD}/../result-envoy-docker"
mkdir -p "${result_dir}"

docker run -it --cap-add SYS_PTRACE --cap-add NET_RAW --cap-add NET_ADMIN \
  -v ${result_dir}:/build -v ${WD}:/build-scripts \
  ubuntu:bionic bash -c "/build-scripts/build_docker_envoy_modsec.sh"
  