# Multi-stage build: compile on Ubuntu, run on distroless as a non-root user.
#   docker build -f deploy/Dockerfile -t hws .
#   docker run -p 8080:8080 -v "$PWD/www:/srv/www:ro" hws
ARG BUILD_IMAGE=ubuntu:24.04
ARG RUNTIME_IMAGE=gcr.io/distroless/base-debian12:nonroot

FROM ${BUILD_IMAGE} AS build
RUN apt-get update && apt-get install -y --no-install-recommends g++ cmake make ca-certificates \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY CMakeLists.txt ./
COPY src ./src
COPY tools ./tools
COPY tests ./tests
COPY fuzz ./fuzz
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DHWS_BUILD_TESTS=OFF -DHWS_BUILD_TOOLS=OFF \
          -DHWS_STATIC_RUNTIME=ON \
    && cmake --build build -j"$(nproc)" \
    && strip build/hws

FROM ${RUNTIME_IMAGE}
COPY --from=build /src/build/hws /usr/local/bin/hws
COPY deploy/hws.conf /etc/hws/hws.conf
EXPOSE 8080
# SIGTERM drains in-flight requests (up to drain-timeout) before exiting 0.
STOPSIGNAL SIGTERM
USER nonroot:nonroot
ENTRYPOINT ["/usr/local/bin/hws", "--config", "/etc/hws/hws.conf"]
