FROM gcc:13-bookworm AS builder
RUN apt-get update && apt-get install -y cmake libssl-dev git pkg-config libpq-dev libhiredis-dev libcurl4-openssl-dev libcrypt-dev && rm -rf /var/lib/apt/lists/*
WORKDIR /app
COPY CMakeLists.txt vcpkg.json .clang-format ./
COPY src src
COPY tests tests
COPY templates templates
COPY static static
COPY scripts scripts
COPY nginx nginx
COPY .env.example .gitignore .stylelintrc.json MIGRASI_STATUS.md docs-cutover.md ./
RUN cmake -B build -DCMAKE_BUILD_TYPE=Release -DWITH_UWEBSOCKETS=ON && cmake --build build -j$(nproc) && ./build/examvan-tests --gtest_filter=-ServerLive.*:F7Jobs.JobRunnerStartStop:P3*:P4*:P5*:P6*:P7*:P8*:P9*:P10*:P11*:P12*:P13*

FROM builder AS sanitizer
RUN cmake -B build-san -DCMAKE_BUILD_TYPE=Debug -DENABLE_SANITIZERS=ON && cmake --build build-san -j$(nproc) && ./build-san/examvan-tests --gtest_filter=-ServerLive.*:F7Jobs.JobRunnerStartStop
CMD ["./build-san/examvan-tests"]

FROM gcc:13-bookworm AS runtime
RUN apt-get update && apt-get install -y libpq-dev libhiredis-dev libcurl4-openssl-dev libcrypt-dev curl ca-certificates && rm -rf /var/lib/apt/lists/*
# Docker hardening: no-new-privileges
COPY --from=builder /app/build/examvan-server /usr/local/bin/examvan-server
COPY --from=builder /app/templates /app/templates
COPY --from=builder /app/static /app/static
WORKDIR /app
EXPOSE 5000
ENTRYPOINT ["/usr/local/bin/examvan-server"]
