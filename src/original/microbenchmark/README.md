## Description
This microbenchmark generates two distinct memory accesses: (1) pointer-chasing and (2) sequential read. The memory access for sequential read is much more intensive than pointer-chasing.
This microbenchmark can be used to run with one thread for pointer-chasing or one thread for sequential read, or both of them. Random pointer-chasing is used by default.

## Usage
* Compile: `make`
* Run pointer-chasing (`-i`: number of iteration, `-A`: data buffer size in MB):
  ```
  ./bench -R 0.0 -i 9 -A 1024
  ```
* Run sequential-read (`-i`: number of iteration, `-B`: data buffer size in MB):
  ```
  ./bench -R 1.0 -i 9 -B 1024
  ```
* Run both of them (`-i`: number of iteration, `-A`: data buffer size in MB for pointer-chasing, `-B`: data buffer size in MB for sequential-read): 
  ```
  ./bench -R 0.5 -i 9 -A 1024 -B 1024
  ```
