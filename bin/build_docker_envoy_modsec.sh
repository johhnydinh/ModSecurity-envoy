#!/usr/bin/env bash
set -e
apt-get update && apt-get install -y \
   wget \
   libtool \
   cmake \
   automake \
   autoconf \
   make \
   ninja-build \
   curl \
   unzip \
   git \
   g++ \
   clang-format-5.0 \
   virtualenv \
   python \
   flex \
   bison \
   doxygen \
   libyajl-dev \
   libgeoip-dev \
   dh-autoreconf \
   libcurl4-gnutls-dev \
   libxml2 \
   libpcre++-dev \
   libxml2-dev

git clone --recurse-submodules -j8 https://github.com/ArthurHlt/ModSecurity-envoy.git
git clone https://github.com/SpiderLabs/ModSecurity.git

cd ModSecurity
git checkout ${MODSEC_VERSION:-v3.0.4}
git submodule update --init
cd ..

cd ModSecurity-envoy/envoy
git pull origin master && git checkout ${ENVOY_VERSION:-v1.16.0}
cd ..


echo "Installing bazel ..."
wget -O /usr/local/bin/bazel https://github.com/bazelbuild/bazelisk/releases/download/v0.0.8/bazelisk-linux-amd64
chmod +x /usr/local/bin/bazel
echo "Finished installing bazel."


echo "Installing golang ..."
wget -qO- https://dl.google.com/go/go1.13.7.linux-amd64.tar.gz | tar -C /usr/local -xzf -

export GOPATH="${HOME}/gobuild"
export PATH="${PATH}:${GOPATH}/bin:/usr/local/go/bin"
echo "Finished installing golang."

echo "Installing buildifier and buildozer ..."
go get -u github.com/bazelbuild/buildtools/buildifier
go get -u github.com/bazelbuild/buildtools/buildozer
echo "Finished installing buildifier and buildozer."
# bazel build --jobs=2 --explain=file.txt --verbose_explanations //source/exe:envoy-static

echo "building modsecurity ..."
cd ../ModSecurity
./build.sh
./configure
make
make install

echo "building envoy..."
cd ../ModSecurity-envoy
nb_cpu=$(python -c 'import multiprocessing as mp; print(mp.cpu_count())')
nb_cpu=$((${nb_cpu}+1))
bazel build --jobs=${nb_cpu} -c opt --copt="-Wno-maybe-uninitialized" --copt="-Wno-uninitialized" --cxxopt="-Wno-uninitialized" //:envoy-static.stripped --config=sizeopt
cp /ModSecurity-envoy/bazel-bin/envoy-static.stripped /build/envoy
chmod 0766 /build/envoy