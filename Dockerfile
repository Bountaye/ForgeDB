FROM ubuntu:24.04 AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
    g++ cmake ninja-build python3 ca-certificates && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY CMakeLists.txt ./
COPY include ./include
COPY src ./src
COPY cli ./cli
COPY benchmarks ./benchmarks
COPY tests ./tests
RUN cmake -S . -B /build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    && cmake --build /build --parallel 4 \
    && ctest --test-dir /build --output-on-failure

FROM ubuntu:24.04 AS runtime
RUN apt-get update && apt-get install -y --no-install-recommends libstdc++6 \
    && rm -rf /var/lib/apt/lists/* \
    && groupadd --system forgedb && useradd --system --gid forgedb forgedb \
    && mkdir -p /var/lib/forgedb && chown forgedb:forgedb /var/lib/forgedb
COPY --from=build /build/forgedb-server /build/forgedb-cli /build/forgedb-benchmark /usr/local/bin/
USER forgedb
VOLUME ["/var/lib/forgedb"]
EXPOSE 6380
STOPSIGNAL SIGTERM
ENTRYPOINT ["forgedb-server"]
CMD ["--host", "0.0.0.0", "--data-dir", "/var/lib/forgedb"]
