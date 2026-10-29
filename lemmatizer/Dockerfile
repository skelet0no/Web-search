FROM debian:bookworm-slim AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake git ca-certificates libicu-dev libssl-dev libboost-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY CMakeLists.txt ./
COPY cmake ./cmake
COPY text ./text
COPY tokenizer ./tokenizer
COPY lemmatizer ./lemmatizer
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/opt/web-search \
    -DTOKENIZER_WITH_MONGO=OFF -DLEMMATIZER_WITH_MONGO=ON \
    && cmake --build build --parallel 4 \
    && ctest --test-dir build --output-on-failure \
    && cmake --install build

FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends \
    libicu72 libssl3 libsnappy1v5 libzstd1 zlib1g ca-certificates \
    && rm -rf /var/lib/apt/lists/*
COPY --from=build /opt/web-search /opt/web-search
ENV LD_LIBRARY_PATH=/opt/web-search/lib
WORKDIR /app
USER 65532:65532
ENTRYPOINT ["/opt/web-search/bin/lemmatizer", "--dictionary", "/opt/web-search/share/web-search/dictionary"]
CMD ["--help"]
