#include "httplib.h"
#include <libpq-fe.h>
#include <mutex>
#include <string>
#include <iostream>

using namespace httplib;

// Global DB connection
PGconn *conn = nullptr;
std::mutex db_mutex;

// Initialize DB
void init_db() {
    conn = PQconnectdb("host=0.0.0.0 port=5432 dbname=kvdb user=postgres password=1234");
    if (PQstatus(conn) != CONNECTION_OK) {
        std::cerr << "DB connection failed\n";
        exit(1);
    }
    
    // Create table
    const char* create_table = 
        "CREATE TABLE IF NOT EXISTS kv_store ("
        "key TEXT PRIMARY KEY, "
        "value TEXT NOT NULL);";
    
    PGresult *res = PQexec(conn, create_table);
    PQclear(res);
    std::cout << "Database ready\n";
}

// PUT: Store in DB
void put_kv(const std::string &key, const std::string &value) {
    std::lock_guard<std::mutex> lock(db_mutex);
    std::string query = 
        "INSERT INTO kv_store (key, value) VALUES ('" + key + "', '" + value + "') "
        "ON CONFLICT (key) DO UPDATE SET value='" + value + "';";
    PGresult *res = PQexec(conn, query.c_str());
    PQclear(res);
}

// GET: Retrieve from DB
std::string get_kv(const std::string &key) {
    std::lock_guard<std::mutex> lock(db_mutex);
    std::string query = "SELECT value FROM kv_store WHERE key='" + key + "';";
    PGresult *res = PQexec(conn, query.c_str());
    
    std::string value;
    if (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) > 0) {
        value = PQgetvalue(res, 0, 0);
    }
    PQclear(res);
    
    return value;
}

// DELETE: Remove from DB
void delete_kv(const std::string &key) {
    std::lock_guard<std::mutex> lock(db_mutex);
    std::string query = "DELETE FROM kv_store WHERE key='" + key + "';";
    PGresult *res = PQexec(conn, query.c_str());
    PQclear(res);
}

int main() {
    init_db();
    
    // Create server with thread pool
    Server svr;
    svr.new_task_queue = [] { return new ThreadPool(8); };
    
    // PUT endpoint
    svr.Post("/put", [](const Request &req, Response &res) {
        auto key = req.get_param_value("key");
        auto value = req.get_param_value("value");
        
        if (key.empty() || value.empty()) {
            res.status = 400;
            res.set_content("Missing key or value\n", "text/plain");
            return;
        }
        
        put_kv(key, value);
        res.set_content("OK\n", "text/plain");
    });
    
    // GET endpoint
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
    
    // DELETE endpoint
    svr.Delete("/delete", [](const Request &req, Response &res) {
        auto key = req.get_param_value("key");
        
        if (key.empty()) {
            res.status = 400;
            res.set_content("Missing key\n", "text/plain");
            return;
        }
        
        delete_kv(key);
        res.set_content("Deleted\n", "text/plain");
    });
    
    std::cout << "Server listening on port 8080...\n";
    svr.listen("0.0.0.0", 8080);
    
    PQfinish(conn);
    return 0;
}
