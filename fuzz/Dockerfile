# Toolchain image for running the libFuzzer targets locally (CI uses the runner's clang).
#   docker build -t hws-fuzz fuzz/
#   docker run --rm -v "$PWD:/src" -w /src hws-fuzz fuzz/run.sh 300
FROM ubuntu:24.04
RUN apt-get update && apt-get install -y --no-install-recommends clang-18 libclang-rt-18-dev cmake make python3 \
    && rm -rf /var/lib/apt/lists/* \
    && ln -s /usr/bin/clang++-18 /usr/local/bin/clang++ && ln -s /usr/bin/clang-18 /usr/local/bin/clang
