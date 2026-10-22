FROM debian:bookworm-slim AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake ca-certificates libicu-dev libssl-dev libsasl2-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY CMakeLists.txt ./
COPY cmake ./cmake
COPY tokenizer ./tokenizer
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/opt/tokenizer \
    && cmake --build build --parallel 4 \
    && ctest --test-dir build --output-on-failure \
    && cmake --install build

FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends \
    libicu72 libssl3 libsasl2-2 libsnappy1v5 libzstd1 zlib1g ca-certificates \
    && rm -rf /var/lib/apt/lists/*
COPY --from=build /opt/tokenizer /opt/tokenizer
ENV LD_LIBRARY_PATH=/opt/tokenizer/lib:/opt/tokenizer/lib64
WORKDIR /app
USER 65532:65532
ENTRYPOINT ["/opt/tokenizer/bin/tokenizer"]
CMD ["--help"]
