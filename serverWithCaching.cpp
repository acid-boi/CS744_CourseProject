// server.cpp
// Behavior: Redis reads concurrent, Redis writes serialized (single mutex).
//           Postgres reads concurrent, Postgres writes concurrent (per-thread PGconn).
// Uses libpq (C API) and redis++ (sw::redis).

#include "httplib.h"
#include <libpq-fe.h>
#include <sw/redis++/redis++.h>

#include <mutex>
#include <string>
#include <iostream>
#include <memory>

using namespace httplib;
using namespace sw::redis;

// ---------------- Configuration ----------------
const std::string PG_CONNINFO = "host=kv_postgres port=5432 dbname=kvdb user=postgres password=1234";
const std::string REDIS_HOST = "kv_redis";
const int REDIS_PORT = 6379;

// ---------------- Globals -----------------------
// We do not use a shared PGconn for runtime DB operations.
// Use a temporary global conn during init only (closed after init).
PGconn *init_conn = nullptr;

// Keep a global Redis pointer only for fallback / compatibility.
// Primary operations use thread-local Redis.
Redis *global_redis = nullptr;

// Redis write lock: serializes SET/DEL so only one thread performs write to Redis at a time.
// This matches your requested policy: Redis writes serialized, Redis reads concurrent.
std::mutex redis_write_mutex;

// ---------------- Thread-local connections --------------
Redis* get_thread_redis() {
    thread_local std::unique_ptr<Redis> t_redis;
    if (!t_redis) {
        ConnectionOptions opts;
        opts.host = REDIS_HOST;
        opts.port = REDIS_PORT;
        opts.socket_timeout = std::chrono::milliseconds(200);
        try {
            t_redis.reset(new Redis(opts));
            // Validate connection (optional)
            t_redis->ping();
        } catch (const sw::redis::Error &err) {
            std::cerr << "[thread " << std::this_thread::get_id()
                      << "] thread-local Redis connect error: " << err.what() << "\n";
            t_redis.reset();
            return nullptr;
        }
    }
    return t_redis.get();
}

PGconn* get_thread_pgconn() {
    thread_local PGconn* t_conn = nullptr;
    if (!t_conn) {
        t_conn = PQconnectdb(PG_CONNINFO.c_str());
        if (PQstatus(t_conn) != CONNECTION_OK) {
            std::cerr << "[thread " << std::this_thread::get_id()
                      << "] thread-local PG connect error: " << PQerrorMessage(t_conn) << "\n";
            PQfinish(t_conn);
            t_conn = nullptr;
            return nullptr;
        }
    }
    return t_conn;
}

// ---------------- Initialization -------------------
void init_db() {
    // Use a temporary connection for schema setup, then close it.
    init_conn = PQconnectdb(PG_CONNINFO.c_str());
    if (PQstatus(init_conn) != CONNECTION_OK) {
        std::cerr << "DB init connection failed: " << PQerrorMessage(init_conn) << std::endl;
        PQfinish(init_conn);
        init_conn = nullptr;
        exit(1);
    }

    const char* create_table =
        "CREATE TABLE IF NOT EXISTS kv_store ("
        "key TEXT PRIMARY KEY, "
        "value TEXT NOT NULL);";
    PGresult *res = PQexec(init_conn, create_table);
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        std::cerr << "Table creation failed: " << PQerrorMessage(init_conn) << std::endl;
    }
    PQclear(res);

    // Close the init connection since runtime uses per-thread connections.
    PQfinish(init_conn);
    init_conn = nullptr;

    std::cout << "Database ready\n";
}

void init_redis() {
    try {
        ConnectionOptions opts;
        opts.host = REDIS_HOST;
        opts.port = REDIS_PORT;
        opts.socket_timeout = std::chrono::milliseconds(200);
        global_redis = new Redis(opts);
        global_redis->ping();
        std::cout << "Connected to Redis (global fallback)\n";
    } catch (const sw::redis::Error &err) {
        std::cerr << "Global Redis init failed: " << err.what()
                  << " (thread-local connections will attempt to connect per thread)\n";
        // Not exiting: thread-local Redis connections will be used by threads.
        global_redis = nullptr;
    }
}

// ---------------- DB helpers (concurrent) --------------
bool exec_nonquery_pg(PGconn* c, const std::string &sql) {
    if (!c) return false;
    PGresult *res = PQexec(c, sql.c_str());
    if (!res) return false;
    ExecStatusType st = PQresultStatus(res);
    PQclear(res);
    return (st == PGRES_COMMAND_OK);
}

int range_count_pg(PGconn* c, int low, int high) {
    if (!c) return -1;

    std::string q =
        "SELECT COUNT(*) FROM kv_store "
        "WHERE value ~ '^[0-9]+$' AND "
        "CAST(value AS INTEGER) BETWEEN " + std::to_string(low) +
        " AND " + std::to_string(high) + ";";

    PGresult *res = PQexec(c, q.c_str());
    int count = -1;

    if (res && PQresultStatus(res) == PGRES_TUPLES_OK) {
        count = std::atoi(PQgetvalue(res, 0, 0));
    }

    if (res) PQclear(res);
    return count;
}



std::string select_kv_pg(PGconn* c, const std::string &key) {
    if (!c) return {};
    // NOTE: using simple string concatenation to keep changes minimal.
    // In production prefer PQexecParams to avoid SQL injection.
    std::string query = "SELECT value FROM kv_store WHERE key='" + key + "';";
    PGresult *res = PQexec(c, query.c_str());
    std::string value;
    if (res && PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) > 0) {
        char* v = PQgetvalue(res, 0, 0);
        if (v) value = std::string(v);
    }
    if (res) PQclear(res);
    return value;
}

// ---------------- Application operations ----------------
void put_kv(const std::string &key, const std::string &value) {
    // Redis write must be serialized per your design.
    try {
        std::lock_guard<std::mutex> lg(redis_write_mutex);
        Redis* tr = get_thread_redis();
        if (tr) {
            tr->set(key, value);
        } else if (global_redis) {
            global_redis->set(key, value);
        } else {
            std::cerr << "No redis available for set\n";
        }
    } catch (const sw::redis::Error &err) {
        std::cerr << "Redis set error in put_kv: " << err.what() << "\n";
    }

    // DB write: use thread-local PG connection (concurrent writes allowed)
    PGconn* pc = get_thread_pgconn();
    if (!pc) {
        // Fallback: create a temporary connection for this write
        PGconn* tmp = PQconnectdb(PG_CONNINFO.c_str());
        if (PQstatus(tmp) == CONNECTION_OK) {
            std::string q = "INSERT INTO kv_store (key, value) VALUES ('" + key + "', '" + value + "') "
                            "ON CONFLICT (key) DO UPDATE SET value='" + value + "';";
            PGresult *r = PQexec(tmp, q.c_str());
            if (r) PQclear(r);
        }
        PQfinish(tmp);
        return;
    }

    std::string q = "INSERT INTO kv_store (key, value) VALUES ('" + key + "', '" + value + "') "
                    "ON CONFLICT (key) DO UPDATE SET value='" + value + "';";
    PGresult *res = PQexec(pc, q.c_str());
    if (res) PQclear(res);
}

std::string get_kv(const std::string &key) {
    // Try cache first (thread-local Redis; no locking)
    try {
        Redis* tr = get_thread_redis();
        if (tr) {
            auto val = tr->get(key);
            if (val) {
                // cache hit
                // Note: avoid excessive log in high load
                std::cout << "Cache hit for key: " << key << std::endl;
                return *val;
            }
        } else if (global_redis) {
            auto val = global_redis->get(key);
            if (val) return *val;
        }
    } catch (const sw::redis::Error &err) {
        std::cerr << "Redis get error: " << err.what() << "\n";
    }

    // Cache miss → DB read using thread-local PGconn (concurrent)
    PGconn* pc = get_thread_pgconn();
    if (!pc) {
        // fallback: temporary connection
        PGconn* tmp = PQconnectdb(PG_CONNINFO.c_str());
        std::string r;
        if (PQstatus(tmp) == CONNECTION_OK) {
            r = select_kv_pg(tmp, key);
        }
        PQfinish(tmp);
        // Attempt to populate cache (best-effort)
        try {
            std::lock_guard<std::mutex> lg(redis_write_mutex);
            Redis* trf = get_thread_redis();
            if (trf) trf->set(key, r);
            else if (global_redis) global_redis->set(key, r);
        } catch (...) {}
        return r;
    }

    std::string db_val = select_kv_pg(pc, key);

    if (!db_val.empty()) {
        // Populate cache (best-effort). Writes to Redis are serialized.
        try {
            std::lock_guard<std::mutex> lg(redis_write_mutex);
            Redis* trf = get_thread_redis();
            if (trf) trf->set(key, db_val);
            else if (global_redis) global_redis->set(key, db_val);
        } catch (const sw::redis::Error &err) {
            std::cerr << "Redis set error while caching: " << err.what() << "\n";
        }
    }
    return db_val;
}

bool delete_kv(const std::string &key) {
    // Redis delete serialized
    try {
        std::lock_guard<std::mutex> lg(redis_write_mutex);
        Redis* tr = get_thread_redis();
        if (tr) tr->del(key);
        else if (global_redis) global_redis->del(key);
    } catch (const sw::redis::Error &err) {
        std::cerr << "Redis del error: " << err.what() << "\n";
    }

    // DB delete using thread-local PGconn (concurrent)
    PGconn* pc = get_thread_pgconn();
    if (!pc) {
        PGconn* tmp = PQconnectdb(PG_CONNINFO.c_str());
        bool deleted = false;
        if (PQstatus(tmp) == CONNECTION_OK) {
            std::string q = "DELETE FROM kv_store WHERE key='" + key + "';";
            PGresult *r = PQexec(tmp, q.c_str());
            if (r) {
                deleted = (PQresultStatus(r) == PGRES_COMMAND_OK && atoi(PQcmdTuples(r)) > 0);
                PQclear(r);
            }
        }
        PQfinish(tmp);
        return deleted;
    }

    std::string q = "DELETE FROM kv_store WHERE key='" + key + "';";
    PGresult *res = PQexec(pc, q.c_str());
    bool deleted = false;
    if (res) {
        deleted = (PQresultStatus(res) == PGRES_COMMAND_OK && atoi(PQcmdTuples(res)) > 0);
        PQclear(res);
    }
    return deleted;
}

// ---------------- HTTP server -------------------------
int main() {
    std::cout << "Starting KV Server (Redis reads concurrent, Redis writes serialized, Postgres concurrent writes)...\n";

    init_db();
    init_redis();

    Server svr;
    // Keep same threadpool size as before
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

    svr.Post("/range_count", [](const Request &req, Response &res) {
        auto low_s  = req.get_param_value("low");
        auto high_s = req.get_param_value("high");

        if (low_s.empty() || high_s.empty()) {
            res.status = 400;
            res.set_content("Missing low or high\n", "text/plain");
            return;
        }

        int low  = std::stoi(low_s);
        int high = std::stoi(high_s);

        PGconn* pc = get_thread_pgconn();
        int count = -1;

        if (pc) {
            count = range_count_pg(pc, low, high);
        } else {
            // fallback temporary connection
            PGconn* tmp = PQconnectdb(PG_CONNINFO.c_str());
            if (PQstatus(tmp) == CONNECTION_OK) {
                count = range_count_pg(tmp, low, high);
            }
            PQfinish(tmp);
        }

        if (count < 0) {
            res.status = 500;
            res.set_content("DB error\n", "text/plain");
            return;
        }

        res.set_content(std::to_string(count) + "\n", "text/plain");
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

    svr.Get("/check_cache", [](const Request &req, Response &res) {
        auto key = req.get_param_value("key");
        if (key.empty()) {
            res.status = 400;
            res.set_content("Missing key\n", "text/plain");
            return;
        }
        try {
            Redis* tr = get_thread_redis();
            if (tr) {
                auto exists = tr->exists(key);
                if (exists > 0) res.set_content("Key exists in cache\n", "text/plain");
                else { res.status = 404; res.set_content("Key not in cache\n", "text/plain"); }
            } else if (global_redis) {
                std::lock_guard<std::mutex> lg(redis_write_mutex);
                auto exists = global_redis->exists(key);
                if (exists > 0) res.set_content("Key exists in cache\n", "text/plain");
                else { res.status = 404; res.set_content("Key not in cache\n", "text/plain"); }
            } else {
                res.status = 500;
                res.set_content("Redis not available\n", "text/plain");
            }
        } catch (const sw::redis::Error &err) {
            res.status = 500;
            res.set_content("Redis error\n", "text/plain");
        }
    });

    std::cout << "Server running on port 8080...\n";
    svr.listen("0.0.0.0", 8080);

    // cleanup
    if (global_redis) delete global_redis;
    return 0;
}

