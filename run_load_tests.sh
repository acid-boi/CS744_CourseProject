#!/bin/bash

# Create CSV file and write header
echo "threads,throughput,latency" > load_test_results.csv

# Loop from 1 to 10 threads
for i in {1..10}
do
  echo "Running load test with $i threads..."
  
  # Run loadgen and capture output
  output=$(taskset -c 3,4,5,6 ./loadgen get_popular --threads $i --popular 1000 --duration 300)
  
  # Parse throughput and latency from output
  throughput=$(echo "$output" | awk '/Throughput:/ {print $2}')
  latency=$(echo "$output" | awk '/Average:/ {print $2}')
  
  # Append results to CSV
  echo "$i,$throughput,$latency" >> load_test_results.csv
done

echo "Load testing complete. Results saved in load_test_results.csv"
