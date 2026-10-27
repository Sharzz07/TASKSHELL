#!/bin/sh
# Generate 1.5 million random numbers (a real data-processing workload).
awk 'BEGIN { srand(7); for (i = 0; i < 1500000; i++) print int(rand() * 1000000000) }' > big.txt
