FROM ubuntu:24.04
RUN apt update && \
    apt install -y g++ make wget libpq-dev git && \
    rm -rf /var/lib/apt/lists/*
RUN git clone --depth 1 https://github.com/yhirose/cpp-httplib.git /opt/cpp-httplib
WORKDIR /app
COPY server.cpp .
RUN g++ -std=c++11 server.cpp -o kv_server \
    -I/opt/cpp-httplib \
    -I/usr/include/postgresql \
    -lpq -pthread
CMD ["./kv_server"]
