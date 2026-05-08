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
