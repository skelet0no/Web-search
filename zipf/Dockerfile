FROM debian:bookworm-slim AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake git ca-certificates libicu-dev libssl-dev pkg-config libcairo2-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY CMakeLists.txt ./
COPY cmake ./cmake
COPY text ./text
COPY tokenizer ./tokenizer
COPY zipf ./zipf
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/opt/web-search \
    -DTOKENIZER_WITH_MONGO=OFF -DLEMMATIZER_BUILD=OFF -DZIPF_BUILD=ON \
    && cmake --build build --target zipf zipf_tests zipf_input_tests --parallel 4 \
    && ctest --test-dir build -R '^zipf_' --output-on-failure \
    && cmake --install build

FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends \
    libcairo2 fonts-dejavu-core libssl3 libsnappy1v5 libzstd1 zlib1g ca-certificates \
    && rm -rf /var/lib/apt/lists/*
COPY --from=build /opt/web-search /opt/web-search
ENV LD_LIBRARY_PATH=/opt/web-search/lib:/opt/web-search/lib64
WORKDIR /app
USER 65532:65532
ENTRYPOINT ["/opt/web-search/bin/zipf"]
