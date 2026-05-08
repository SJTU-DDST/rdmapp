# rdmapp benchmark results

Date: 2026-05-02

## Environment

- Local host: `192.168.98.70`
- Remote host: `192.168.98.74`
- Remote access: `ssh yitian@192.168.98.74`
- Repository path on both hosts: `ddst/rdmapp`
- Commit tested: `5232bc0acf5b600364995dc45065f602796fa8c5`
- Build mode: xmake release
- NUMA binding: both client and server were run with `numactl -N 0 -m 0`
- Test direction: remote host ran the server, local host ran the client

## Commands

Latency server:

```sh
numactl -N 0 -m 0 ./build/linux/x86_64/release/latency --payload-size <size> --count <count> <port>
```

Latency client:

```sh
numactl -N 0 -m 0 ./build/linux/x86_64/release/latency --payload-size <size> --count <count> 192.168.98.74 <port>
```

send_bw server:

```sh
numactl -N 0 -m 0 ./build/linux/x86_64/release/send_bw --payload-size <size> --count <count> <port>
```

send_bw client:

```sh
numactl -N 0 -m 0 ./build/linux/x86_64/release/send_bw --payload-size <size> --count <count> 192.168.98.74 <port>
```

## latency

| Payload | Count | write_with_imm/recv avg latency | send/recv avg latency |
| --- | ---: | ---: | ---: |
| 64 B | 100000 | 4.429 us | 5.528 us |
| 256 B | 100000 | 4.854 us | 4.749 us |
| 512 B | 100000 | 4.652 us | 4.596 us |
| 1 KiB | 100000 | 3.723 us | 3.907 us |
| 2 KiB | 100000 | 3.822 us | 4.191 us |
| 4 KiB | 100000 | 4.669 us | 4.202 us |
| 64 KiB | 50000 | 8.523 us | 11.067 us |
| 1 MiB | 5000 | 76.209 us | 78.607 us |
| 2 MiB | 2000 | 147.509 us | 151.809 us |

## send_bw

The table uses the server-side reporter output. The final partial one-second
window after the client finished was excluded from the average.

| Payload | Count per worker | Stable samples | Avg IOPS | Avg bandwidth |
| --- | ---: | ---: | ---: | ---: |
| 64 B | 2000000 | 6 | 1260874.30 ops/s | 0.65 Gbps |
| 256 B | 2000000 | 6 | 1199255.35 ops/s | 2.46 Gbps |
| 512 B | 2000000 | 7 | 1142694.12 ops/s | 4.68 Gbps |
| 1 KiB | 2000000 | 7 | 1142684.46 ops/s | 9.36 Gbps |
| 2 KiB | 2000000 | 7 | 1124300.65 ops/s | 18.42 Gbps |
| 4 KiB | 2000000 | 8 | 965857.19 ops/s | 31.65 Gbps |
| 64 KiB | 500000 | 6 | 329757.53 ops/s | 172.89 Gbps |
| 1 MiB | 20000 | 3 | 23218.21 ops/s | 194.77 Gbps |
| 2 MiB | 10000 | 3 | 11640.17 ops/s | 195.29 Gbps |

Client-side per-worker average send completion latency:

| Payload | Count per worker | Worker 0 | Worker 1 | Worker 2 | Worker 3 |
| --- | ---: | ---: | ---: | ---: | ---: |
| 64 B | 2000000 | 3.172 us | 3.172 us | 3.172 us | 3.172 us |
| 256 B | 2000000 | 3.335 us | 3.335 us | 3.335 us | 3.335 us |
| 512 B | 2000000 | 3.425 us | 3.425 us | 3.425 us | 3.425 us |
| 1 KiB | 2000000 | 3.436 us | 3.436 us | 3.436 us | 3.436 us |
| 2 KiB | 2000000 | 3.558 us | 3.558 us | 3.558 us | 3.558 us |
| 4 KiB | 2000000 | 4.141 us | 4.141 us | 4.141 us | 4.141 us |
| 64 KiB | 500000 | 12.135 us | 12.135 us | 12.135 us | 12.135 us |
| 1 MiB | 20000 | 173.515 us | 173.498 us | 173.481 us | 173.462 us |
| 2 MiB | 10000 | 347.532 us | 347.438 us | 347.332 us | 347.218 us |

## 4 MiB Thread Scaling

Date: 2026-05-08

- Commit tested: `f07370ed3dc0891600aff4ef2ba0076784415c1b`
- Payload: 4 MiB
- Thread definition: one independent QP / benchmark stream. CQ poller scheduler threads are reported separately and are not counted as benchmark threads.
- Scheduler policy: `scheduler_threads = ceil(threads / 4)`, keeping at least one scheduler per four benchmark streams.
- NUMA binding: both client and server used `numactl -N 0 -m 0`.
- Direction: remote host `192.168.98.74` ran the server, local host `192.168.98.70` ran the client.
- `send_bw` uses send depth per benchmark thread = 1 and `--recv-depth 2`.

### latency

Each latency thread is one independent QP and runs serially with outstanding
depth 1. The values below are averages across all per-QP server-side averages
for that thread count.

| Threads | Scheduler threads | Count per thread | write_with_imm/recv avg latency | send/recv avg latency |
| ---: | ---: | ---: | ---: | ---: |
| 1 | 1 | 200 | 291.840 us | 303.465 us |
| 2 | 1 | 200 | 560.337 us | 576.942 us |
| 4 | 1 | 200 | 1113.475 us | 1131.636 us |
| 8 | 2 | 200 | 2227.097 us | 2233.059 us |
| 12 | 3 | 200 | 3342.562 us | 3346.392 us |
| 16 | 4 | 200 | 4471.629 us | 4479.440 us |
| 24 | 6 | 200 | 6550.684 us | 6478.793 us |
| 32 | 8 | 200 | 8693.676 us | 8555.000 us |

### send_bw

The bandwidth table uses the server-side reporter output. To keep the one-second
reporter from undercounting short runs, this sweep used approximately 60000
total sends per thread count: `count_per_thread = 60000 / threads`. Startup/ramp
windows and final partial windows were excluded; in this run the stable plateau
windows are the samples at or above 5500 ops/s. The client latency column is the
average of per-thread send completion averages.

| Threads | Scheduler threads | Count per thread | Stable samples | Avg IOPS | Avg bandwidth | Client avg send completion |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 1 | 60000 | 9 | 5621.28 ops/s | 188.62 Gbps | 177.908 us |
| 2 | 1 | 30000 | 9 | 5831.23 ops/s | 195.66 Gbps | 342.908 us |
| 4 | 1 | 15000 | 9 | 5830.89 ops/s | 195.65 Gbps | 685.394 us |
| 8 | 2 | 7500 | 9 | 5825.35 ops/s | 195.47 Gbps | 1370.880 us |
| 12 | 3 | 5000 | 9 | 5830.06 ops/s | 195.62 Gbps | 2041.949 us |
| 16 | 4 | 3750 | 9 | 5830.70 ops/s | 195.64 Gbps | 2713.157 us |
| 24 | 6 | 2500 | 9 | 5816.29 ops/s | 195.16 Gbps | 3934.840 us |
| 32 | 8 | 1875 | 10 | 5677.38 ops/s | 190.50 Gbps | 5481.768 us |

## 1.5 KiB Latency Thread Scaling

Date: 2026-05-08

- Commit tested: `766e4eef3c25cc3435c4337a233ae9480362fe8d`
- Payload: 1536 bytes
- Thread definition: one independent QP / benchmark stream. CQ poller scheduler threads are reported separately and are not counted as benchmark threads.
- Scheduler policy: `scheduler_threads = ceil(threads / 4)`, keeping at least one scheduler per four benchmark streams.
- Count per thread: 100000
- NUMA binding: both client and server used `numactl -N 0 -m 0`.
- Direction: remote host `192.168.98.74` ran the server, local host `192.168.98.70` ran the client.

The values below are server-side averages. For each thread count, the average is
computed across all per-QP averages; the range shows the minimum and maximum
per-QP average in that run.

| Threads | Scheduler threads | write_with_imm/recv avg latency | write range | send/recv avg latency | send range |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 1 | 7.112 us | 7.112..7.112 us | 7.170 us | 7.170..7.170 us |
| 2 | 1 | 4.649 us | 4.359..4.939 us | 4.614 us | 4.501..4.728 us |
| 4 | 1 | 5.913 us | 5.196..6.757 us | 7.187 us | 6.174..8.069 us |
| 8 | 2 | 7.698 us | 6.750..8.644 us | 8.064 us | 6.875..8.774 us |
| 12 | 3 | 5.978 us | 4.569..8.367 us | 5.402 us | 4.812..5.908 us |
| 16 | 4 | 6.487 us | 5.383..9.188 us | 6.839 us | 5.586..9.612 us |
| 24 | 6 | 12.836 us | 9.063..20.480 us | 12.389 us | 9.714..15.653 us |
| 32 | 8 | 36.357 us | 23.686..60.491 us | 33.953 us | 20.871..53.000 us |

## 1.5 KiB Latency Process Scaling

Date: 2026-05-08

- Commit tested: `293bc37c1765abdc0251382d335d87eb2c75c939`
- Payload: 1536 bytes
- Process definition: one independent `latency` process. Each process uses `--threads 1 --scheduler-threads 1`.
- Count per process: 100000
- NUMA binding: both client and server processes used `numactl -N 0 -m 0`.
- Direction: remote host `192.168.98.74` ran the server processes, local host `192.168.98.70` ran the client processes.

The values below are server-side averages. For each process count, the average
is computed across all per-process averages; the range shows the minimum and
maximum per-process average in that run.

| Processes | Threads per process | Count per process | write_with_imm/recv avg latency | write range | send/recv avg latency | send range |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 1 | 100000 | 3.565 us | 3.565..3.565 us | 3.935 us | 3.935..3.935 us |
| 2 | 1 | 100000 | 3.595 us | 3.590..3.600 us | 4.613 us | 4.549..4.677 us |
| 4 | 1 | 100000 | 3.966 us | 3.853..4.086 us | 4.370 us | 4.115..4.642 us |
| 8 | 1 | 100000 | 4.710 us | 4.495..5.222 us | 5.321 us | 5.023..5.678 us |
| 12 | 1 | 100000 | 6.184 us | 5.088..7.689 us | 6.100 us | 4.993..7.520 us |
| 16 | 1 | 100000 | 9.226 us | 5.923..16.173 us | 9.752 us | 8.683..10.954 us |
| 24 | 1 | 100000 | 31.719 us | 9.718..58.720 us | 18.129 us | 9.267..30.312 us |
| 32 | 1 | 100000 | 104.177 us | 49.501..200.366 us | 53.216 us | 26.705..105.053 us |
