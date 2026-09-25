# Reference Linux environment: GCC 13 and Clang 18 (with libFuzzer, sanitizers,
# clang-tidy, clang-format and llvm-cov) on Ubuntu 24.04.
#
#   docker build -t order-matcher .
#   docker run --rm order-matcher                      # full scripts/check.sh
#   docker run --rm -i order-matcher build/release/matcher < data/golden/brief_example.in
FROM ubuntu:24.04

RUN apt-get update \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      build-essential g++-13 cmake ninja-build git ca-certificates \
      clang-18 clang-tidy-18 clang-format-18 llvm-18 libclang-rt-18-dev \
      libgtest-dev \
 && rm -rf /var/lib/apt/lists/*

ENV PATH=/usr/lib/llvm-18/bin:$PATH \
    CC=gcc-13 CXX=g++-13

WORKDIR /src
COPY . .

# A ready-to-run release build so the image can also just run the matcher.
RUN cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release \
 && cmake --build build/release

CMD ["scripts/check.sh"]
