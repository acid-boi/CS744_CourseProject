g++ -std=c++11 server.cpp -o kv_server \
    -I/opt/cpp-httplib \
    -I/usr/include/postgresql \
    -lpq -pthread
