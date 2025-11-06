FROM ubuntu:24.04

RUN apt update && \
    apt install -y g++ make git cmake libpq-dev libhiredis-dev && \
    rm -rf /var/lib/apt/lists/*

# Clone cpp-httplib
RUN git clone --depth 1 https://github.com/yhirose/cpp-httplib.git /opt/cpp-httplib

# Build redis-plus-plus
WORKDIR /tmp
RUN git clone --depth 1 https://github.com/sewenew/redis-plus-plus.git && \
    cd redis-plus-plus && mkdir build && cd build && \
    cmake -DREDIS_PLUS_PLUS_BUILD_TEST=OFF -DREDIS_PLUS_PLUS_BUILD_SHARED=OFF .. && \
    make -j$(nproc) && make install

# Build your server
WORKDIR /app
COPY serverWithCaching.cpp .
RUN g++ serverWithCaching.cpp -o kv_server \
    -I/opt/cpp-httplib \
    -I/usr/include/postgresql \
    -lpq -lhiredis -lredis++ -pthread

CMD ["./kv_server"]

