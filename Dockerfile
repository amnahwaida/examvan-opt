FROM gcc:13-bookworm AS builder
RUN apt-get update && apt-get install -y cmake libssl-dev git pkg-config libpq-dev libhiredis-dev libcurl4-openssl-dev && rm -rf /var/lib/apt/lists/*
WORKDIR /app
COPY CMakeLists.txt vcpkg.json .clang-format ./
COPY src src
COPY tests tests
COPY templates templates
COPY static static
RUN cmake -B build -DCMAKE_BUILD_TYPE=Release -DWITH_UWEBSOCKETS=OFF && cmake --build build -j$(nproc) && ./build/examvan-tests

FROM gcr.io/distroless/cc-debian12:nonroot AS runtime
COPY --from=builder /app/build/examvan-server /usr/local/bin/examvan-server
COPY --from=builder /app/templates /app/templates
COPY --from=builder /app/static /app/static
EXPOSE 5000
USER nonroot
ENTRYPOINT ["/usr/local/bin/examvan-server"]

FROM builder AS sanitizer
RUN cmake -B build-san -DCMAKE_BUILD_TYPE=Debug -DENABLE_SANITIZERS=ON && cmake --build build-san -j$(nproc) && ./build-san/examvan-tests
CMD ["./build-san/examvan-tests"]
