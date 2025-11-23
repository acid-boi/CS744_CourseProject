// simple_loadgen.cpp

#include "httplib.h"
#include <iostream>
#include <thread>
#include <atomic>
#include <vector>
#include <chrono>
#include <random>
#include <mutex>
#include <algorithm>

using namespace std;

// Global stats
atomic<int> total_requests(0);
atomic<int> successful_requests(0);
atomic<int> failed_requests(0);

// Latency tracking
vector<double> latencies;
mutex latency_mutex;

// Configuration
string host = "127.0.0.1";
int port = 8080;
int num_threads = 8;
int duration_seconds = 60;
int num_popular_keys = 1000;
int keyspace_size = 100000;

// Record latency in milliseconds
void record_latency(double latency_ms) {
    lock_guard<mutex> lock(latency_mutex);
    latencies.push_back(latency_ms);
}

// Seed keys before running get_popular workload
void seed_keys(int count) {
    cout << "Seeding " << count << " keys..." << endl;
    httplib::Client client(host, port);
    
    for (int i = 0; i < count; i++) {
        string key = "k_" + to_string(i);
        string value = "v_" + to_string(i);
        string body = "key=" + key + "&value=" + value;
        
        client.Post("/put", body, "application/x-www-form-urlencoded");
        
        if (i % 1000 == 0 && i > 0) {
            cout << "Seeded " << i << " keys..." << endl;
        }
    }
    cout << "Seeding complete!" << endl;
}

// Worker for GET requests on popular keys (hot cache)
void worker_get_popular(atomic<bool>& stop) {
    httplib::Client client(host, port);
    random_device rd;
    mt19937 gen(rd());
    uniform_int_distribution<> dist(0, num_popular_keys - 1);
    
    while (!stop) {
        int key_id = dist(gen);
        string path = "/get?key=k_" + to_string(key_id);
        
        auto start = chrono::high_resolution_clock::now();
        auto res = client.Get(path);
        auto end = chrono::high_resolution_clock::now();
        
        double latency_ms = chrono::duration<double, milli>(end - start).count();
        record_latency(latency_ms);
        
        total_requests++;
        
        if (res && res->status == 200) {
            successful_requests++;
        } else {
            failed_requests++;
        }
    }
}

// Worker for GET requests across entire keyspace (cold cache)
void worker_get_all(atomic<bool>& stop) {
    httplib::Client client(host, port);
    random_device rd;
    mt19937 gen(rd());
    uniform_int_distribution<> dist(0, keyspace_size - 1);
    
    while (!stop) {
        int key_id = dist(gen);
        string path = "/get?key=k_" + to_string(key_id);
        
        auto start = chrono::high_resolution_clock::now();
        auto res = client.Get(path);
        auto end = chrono::high_resolution_clock::now();
        
        double latency_ms = chrono::duration<double, milli>(end - start).count();
        record_latency(latency_ms);
        
        total_requests++;
        
        if (res && res->status == 200) {
            successful_requests++;
        } else {
            failed_requests++;
        }
    }
}

// Worker for PUT requests
void worker_put(atomic<bool>& stop) {
    httplib::Client client(host, port);
    random_device rd;
    mt19937 gen(rd());
    uniform_int_distribution<> dist(0, keyspace_size - 1);
    
    while (!stop) {
        int key_id = dist(gen);
        string key = "k_" + to_string(key_id);
        string value = "v_" + to_string(key_id);
        string body = "key=" + key + "&value=" + value;
        
        auto start = chrono::high_resolution_clock::now();
        auto res = client.Post("/put", body, "application/x-www-form-urlencoded");
        auto end = chrono::high_resolution_clock::now();
        
        double latency_ms = chrono::duration<double, milli>(end - start).count();
        record_latency(latency_ms);
        
        total_requests++;
        
        if (res && res->status == 200) {
            successful_requests++;
        } else {
            failed_requests++;
        }
    }
}

// Worker for mixed workload (80% GET, 15% PUT, 5% DELETE)
void worker_mixed(atomic<bool>& stop) {
    httplib::Client client(host, port);
    random_device rd;
    mt19937 gen(rd());
    uniform_int_distribution<> key_dist(0, keyspace_size - 1);
    uniform_int_distribution<> op_dist(1, 100);
    
    while (!stop) {
        int key_id = key_dist(gen);
        int op_choice = op_dist(gen);
        
        auto start = chrono::high_resolution_clock::now();
        
        if (op_choice <= 80) {
            // GET
            string path = "/get?key=k_" + to_string(key_id);
            auto res = client.Get(path);
            auto end = chrono::high_resolution_clock::now();
            double latency_ms = chrono::duration<double, milli>(end - start).count();
            record_latency(latency_ms);
            
            total_requests++;
            if (res && res->status == 200) successful_requests++;
            else failed_requests++;
            
        } else if (op_choice <= 95) {
            // PUT
            string body = "key=k_" + to_string(key_id) + "&value=v_" + to_string(key_id);
            auto res = client.Post("/put", body, "application/x-www-form-urlencoded");
            auto end = chrono::high_resolution_clock::now();
            double latency_ms = chrono::duration<double, milli>(end - start).count();
            record_latency(latency_ms);
            
            total_requests++;
            if (res && res->status == 200) successful_requests++;
            else failed_requests++;
            
        } else {
            // DELETE
            string path = "/delete?key=k_" + to_string(key_id);
            auto res = client.Delete(path);
            auto end = chrono::high_resolution_clock::now();
            double latency_ms = chrono::duration<double, milli>(end - start).count();
            record_latency(latency_ms);
            
            total_requests++;
            if (res && (res->status == 200 || res->status == 404)) successful_requests++;
            else failed_requests++;
        }
    }
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        cout << "Usage: " << argv[0] << " <workload> [options]" << endl;
        cout << "Workloads:" << endl;
        cout << "  get_popular  - GET on hot keys (seeds first)" << endl;
        cout << "  get          - GET on random keys" << endl;
        cout << "  put          - PUT to random keys" << endl;
        cout << "  mixed        - 80% GET, 15% PUT, 5% DELETE" << endl;
        cout << "\nOptions:" << endl;
        cout << "  --threads N    (default: 8)" << endl;
        cout << "  --duration N   (default: 60 seconds)" << endl;
        cout << "  --popular N    (default: 1000 keys)" << endl;
        cout << "  --keyspace N   (default: 100000)" << endl;
        return 1;
    }
    
    string workload = argv[1];
    
    // Parse options
    for (int i = 2; i < argc; i++) {
        string arg = argv[i];
        if (arg == "--threads" && i + 1 < argc) {
            num_threads = stoi(argv[++i]);
        } else if (arg == "--duration" && i + 1 < argc) {
            duration_seconds = stoi(argv[++i]);
        } else if (arg == "--popular" && i + 1 < argc) {
            num_popular_keys = stoi(argv[++i]);
        } else if (arg == "--keyspace" && i + 1 < argc) {
            keyspace_size = stoi(argv[++i]);
        }
    }
    
    cout << "Configuration:" << endl;
    cout << "  Host: " << host << ":" << port << endl;
    cout << "  Workload: " << workload << endl;
    cout << "  Threads: " << num_threads << endl;
    cout << "  Duration: " << duration_seconds << "s" << endl;
    
    // Seed if needed
    if (workload == "get_popular") {
        seed_keys(num_popular_keys);
    }
    
    // Start workers
    atomic<bool> stop(false);
    vector<thread> workers;
    
    cout << "\nStarting load test..." << endl;
    auto start_time = chrono::steady_clock::now();
    
    for (int i = 0; i < num_threads; i++) {
        if (workload == "get_popular") {
            workers.push_back(thread(worker_get_popular, ref(stop)));
        } else if (workload == "get") {
            workers.push_back(thread(worker_get_all, ref(stop)));
        } else if (workload == "put") {
            workers.push_back(thread(worker_put, ref(stop)));
        } else if (workload == "mixed") {
            workers.push_back(thread(worker_mixed, ref(stop)));
        }
    }
    
    // Run for specified duration
    this_thread::sleep_for(chrono::seconds(duration_seconds));
    stop = true;
    
    // Wait for workers
    for (auto& t : workers) {
        t.join();
    }
    
    auto end_time = chrono::steady_clock::now();
    double elapsed = chrono::duration<double>(end_time - start_time).count();
    
    // Calculate latency stats
    sort(latencies.begin(), latencies.end());
    double avg_latency = 0;
    for (double lat : latencies) {
        avg_latency += lat;
    }
    avg_latency /= latencies.size();
    
    double median_latency = latencies[latencies.size() / 2];
    
    // Print results
    cout << "\n=== Results ===" << endl;
    cout << "Total requests: " << total_requests << endl;
    cout << "Successful: " << successful_requests << endl;
    cout << "Failed: " << failed_requests << endl;
    cout << "Throughput: " << (total_requests / elapsed) << " req/s" << endl;
    cout << "\nLatency (ms):" << endl;
    cout << "  Average: " << avg_latency << endl;
    cout << "  Median: " << median_latency << endl;
    
    return 0;
}
