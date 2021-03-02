#!/usr/bin/env bash
set -e

apt update && apt install -y \
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
   gnupg \
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

curl -fsSL https://bazel.build/bazel-release.pub.gpg | gpg --dearmor > /etc/apt/trusted.gpg.d/bazel.gpg

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
echo "deb [arch=amd64] https://storage.googleapis.com/bazel-apt stable jdk1.8" | tee /etc/apt/sources.list.d/bazel.list
apt update && apt install -y bazel=3.7.2
echo "Finished installing bazel."

echo "Installing LLVM ..."
mkdir /llvm
wget -qO- https://github.com/llvm/llvm-project/releases/download/llvmorg-11.0.1/clang+llvm-11.0.1-x86_64-linux-gnu-ubuntu-16.04.tar.xz | tar -C /llvm -xJf -
llvm_path="$(realpath /llvm/*/)"
echo "Finished installing llvm."

echo "Installing golang ..."
wget -qO- https://dl.google.com/go/go1.15.2.linux-amd64.tar.gz | tar -C /usr/local -xzf -

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
git apply "/build-scripts/modsec-see-errors.patch"
./build.sh
./configure
make
make install

echo "building envoy..."
cd ../ModSecurity-envoy

pushd "./envoy"
git apply "/build-scripts/build.patch"
popd

nb_cpu=$(python -c 'import multiprocessing as mp; print(mp.cpu_count())')
nb_cpu=$((${nb_cpu}+1))

envoy/bazel/setup_clang.sh ${llvm_path}

echo "build --config=clang" >> user.bazelrc
bazel build --jobs=${nb_cpu} -c opt --copt="-Wno-uninitialized" --cxxopt="-Wno-uninitialized" //:envoy-static.stripped --config=sizeopt
cp /ModSecurity-envoy/bazel-bin/envoy-static.stripped /build/envoy
chmod 0766 /build/envoy