#include "httplib.h"
#include <libpq-fe.h>
#include <sw/redis++/redis++.h>
#include <mutex>
#include <string>
#include <iostream>

using namespace httplib;
using namespace sw::redis;

// ----------------- Globals --------------------------------------------------
PGconn *conn = nullptr;
Redis *redis = nullptr;
std::mutex db_mutex;
std::mutex redis_mutex;  // Protect Redis operations

// ----------------- Init DB -----------------------------------------------------------
void init_db() {
    conn = PQconnectdb("host=kv_postgres port=5432 dbname=kvdb user=postgres password=1234");
    if (PQstatus(conn) != CONNECTION_OK) {
        std::cerr << "DB connection failed: " << PQerrorMessage(conn) << std::endl;
        exit(1);
    }

    const char* create_table =
        "CREATE TABLE IF NOT EXISTS kv_store ("
        "key TEXT PRIMARY KEY, "
        "value TEXT NOT NULL);";
    PGresult *res = PQexec(conn, create_table);
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        std::cerr << "Table creation failed: " << PQerrorMessage(conn) << std::endl;
    }
    PQclear(res);
    std::cout << "Database ready\n";
}

// ----------------- Init Redis -----------------
void init_redis() {
    try {
        ConnectionOptions opts;
        opts.host = "kv_redis";
        opts.port = 6379;
        opts.socket_timeout = std::chrono::milliseconds(100);
        
        redis = new Redis(opts);
        redis->ping();
        std::cout << "Connected to Redis\n";
    } catch (const sw::redis::Error &err) {
        std::cerr << "Redis connection failed: " << err.what() << std::endl;
        exit(1);
    }
}

// ----------------- DB Functions -----------------
void put_kv_db(const std::string &key, const std::string &value) {
    std::lock_guard<std::mutex> lock(db_mutex);
    std::string query =
        "INSERT INTO kv_store (key, value) VALUES ('" + key + "', '" + value + "') "
        "ON CONFLICT (key) DO UPDATE SET value='" + value + "';";
    PGresult *res = PQexec(conn, query.c_str());
    PQclear(res);
}

std::string get_kv_db(const std::string &key) {
    std::lock_guard<std::mutex> lock(db_mutex);
    std::string query = "SELECT value FROM kv_store WHERE key='" + key + "';";
    PGresult *res = PQexec(conn, query.c_str());
    std::string value;
    if (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) > 0)
        value = PQgetvalue(res, 0, 0);
    PQclear(res);
    return value;
}

bool delete_kv_db(const std::string &key) {
    std::lock_guard<std::mutex> lock(db_mutex);
    std::string query = "DELETE FROM kv_store WHERE key='" + key + "';";
    PGresult *res = PQexec(conn, query.c_str());
    bool deleted = (PQresultStatus(res) == PGRES_COMMAND_OK && atoi(PQcmdTuples(res)) > 0);
    PQclear(res);
    return deleted;
}

// ----------------- Cache-aware Handlers (Write-Through) -----------------
void put_kv(const std::string &key, const std::string &value) {
    // Lock both to ensure atomicity and prevent cache-DB inconsistency
    std::lock_guard<std::mutex> redis_lock(redis_mutex);
    std::lock_guard<std::mutex> db_lock(db_mutex);
    
    try {
        redis->set(key, value);
    } catch (const sw::redis::Error &err) {
        std::cerr << "Redis error in put_kv: " << err.what() << std::endl;
    }
    
    // Write to DB (inside same critical section)
    std::string query =
        "INSERT INTO kv_store (key, value) VALUES ('" + key + "', '" + value + "') "
        "ON CONFLICT (key) DO UPDATE SET value='" + value + "';";
    PGresult *res = PQexec(conn, query.c_str());
    PQclear(res);
}

std::string get_kv(const std::string &key) {
    // Check cache first (with lock)
    {
        std::lock_guard<std::mutex> lock(redis_mutex);
        try {
            auto val = redis->get(key);
            if (val) {
                std::cout << "Cache hit for key: " << key << std::endl;
                return *val;
            }
        } catch (const sw::redis::Error &err) {
            std::cerr << "Redis error in get_kv: " << err.what() << std::endl;
        }
    }

    // Cache miss -> DB lookup
    std::cout << "Cache miss for key: " << key << std::endl;
    auto db_val = get_kv_db(key);
    
    if (!db_val.empty()) {
        // Populate cache (with lock)
        std::lock_guard<std::mutex> lock(redis_mutex);
        try {
            redis->set(key, db_val);
        } catch (const sw::redis::Error &err) {
            std::cerr << "Redis error caching value: " << err.what() << std::endl;
        }
    }
    return db_val;
}

bool delete_kv(const std::string &key) {
    // Lock both to ensure atomicity
    std::lock_guard<std::mutex> redis_lock(redis_mutex);
    std::lock_guard<std::mutex> db_lock(db_mutex);
    
    try {
        redis->del(key);
    } catch (const sw::redis::Error &err) {
        std::cerr << "Redis error in delete_kv: " << err.what() << std::endl;
    }
    
    std::string query = "DELETE FROM kv_store WHERE key='" + key + "';";
    PGresult *res = PQexec(conn, query.c_str());
    bool deleted = (PQresultStatus(res) == PGRES_COMMAND_OK && atoi(PQcmdTuples(res)) > 0);
    PQclear(res);
    return deleted;
}

// ----------------- main() -----------------
int main() {
    std::cout << "Starting KV Server with Redis cache...\n";
    
    init_db();
    init_redis();

    Server svr;
    svr.new_task_queue = [] { return new ThreadPool(8); };

    svr.Post("/put", [](const Request &req, Response &res) {
        auto key = req.get_param_value("key");
        auto value = req.get_param_value("value");
        if (key.empty() || value.empty()) {
            res.status = 400;
            res.set_content("Missing key or value\n", "text/plain");
            return;
        }
        put_kv(key, value);
        res.set_content("Stored\n", "text/plain");
    });

    svr.Get("/get", [](const Request &req, Response &res) {
        auto key = req.get_param_value("key");
        if (key.empty()) {
            res.status = 400;
            res.set_content("Missing key\n", "text/plain");
            return;
        }
        std::string value = get_kv(key);
        if (value.empty()) {
            res.status = 404;
            res.set_content("Not found\n", "text/plain");
        } else {
            res.set_content(value + "\n", "text/plain");
        }
    });

    svr.Delete("/delete", [](const Request &req, Response &res) {
        auto key = req.get_param_value("key");
        if (key.empty()) {
            res.status = 400;
            res.set_content("Missing key\n", "text/plain");
            return;
        }
        if (delete_kv(key)) {
            res.set_content("Deleted\n", "text/plain");
        } else {
            res.status = 404;
            res.set_content("Not found\n", "text/plain");
        }
    });

    std::cout << "Server running on port 8080...\n";
    svr.listen("0.0.0.0", 8080);

    PQfinish(conn);
    delete redis;
    return 0;
}
