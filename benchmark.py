#!/usr/bin/env python3
"""Measure client-observed request latency against this Redis server."""

import argparse
import concurrent.futures
import socket
import statistics
import struct
import threading
import time


def read_exact(sock, size):
    data = bytearray()
    while len(data) < size:
        chunk = sock.recv(size - len(data))
        if not chunk:
            raise ConnectionError("server closed the connection")
        data.extend(chunk)
    return bytes(data)


def read_value(data, offset=0):
    if offset >= len(data):
        raise ValueError("empty response value")

    tag = data[offset]
    offset += 1
    if tag == 0:
        return tag, None, offset
    if tag == 1:
        code, size = struct.unpack_from("<II", data, offset)
        offset += 8
        message = data[offset:offset + size].decode(errors="replace")
        if offset + size > len(data):
            raise ValueError("truncated error response")
        return tag, (code, message), offset + size
    if tag == 2:
        size = struct.unpack_from("<I", data, offset)[0]
        offset += 4
        if offset + size > len(data):
            raise ValueError("truncated string response")
        return tag, data[offset:offset + size], offset + size
    if tag == 3:
        value = struct.unpack_from("<q", data, offset)[0]
        return tag, value, offset + 8
    if tag == 4:
        value = struct.unpack_from("<d", data, offset)[0]
        return tag, value, offset + 8
    if tag == 5:
        count = struct.unpack_from("<I", data, offset)[0]
        offset += 4
        values = []
        for _ in range(count):
            _, value, offset = read_value(data, offset)
            values.append(value)
        return tag, values, offset
    raise ValueError(f"unknown response tag {tag}")


def request(sock, *args):
    body = bytearray(struct.pack("<I", len(args)))
    for arg in args:
        encoded = arg if isinstance(arg, bytes) else arg.encode()
        body.extend(struct.pack("<I", len(encoded)))
        body.extend(encoded)

    sock.sendall(struct.pack("<I", len(body)) + body)
    response_size = struct.unpack("<I", read_exact(sock, 4))[0]
    response = read_exact(sock, response_size)
    tag, value, offset = read_value(response)
    if offset != len(response):
        raise ValueError("unexpected trailing data in response")
    if tag == 1:
        raise RuntimeError(f"server error {value[0]}: {value[1]}")
    return tag, value


def run_worker(sock, worker_id, request_count, mode, value, start_barrier):
    latencies = []
    key = f"benchmark:{worker_id}"
    start_barrier.wait()

    for index in range(request_count):
        if mode == "set":
            command = ("set", f"{key}:{index}", value)
            expected_tag = 0
        elif mode == "mixed" and index % 2 == 0:
            command = ("set", key, value)
            expected_tag = 0
        else:
            command = ("get", key)
            expected_tag = None

        before = time.perf_counter_ns()
        tag, _ = request(sock, *command)
        elapsed = time.perf_counter_ns() - before
        if expected_tag is not None and tag != expected_tag:
            raise RuntimeError(
                f"worker {worker_id}, request {index}: unexpected response tag {tag}"
            )
        if mode == "get" and tag not in (0, 2):
            raise RuntimeError(
                f"worker {worker_id}, request {index}: GET returned tag {tag}"
            )
        if mode == "mixed" and index % 2 == 1 and tag not in (0, 2):
            raise RuntimeError(
                f"worker {worker_id}, request {index}: GET returned tag {tag}"
            )
        latencies.append(elapsed)
    return latencies


def percentile(sorted_samples, percent):
    index = max(0, (len(sorted_samples) * percent + 99) // 100 - 1)
    return sorted_samples[index] / 1_000


def main():
    parser = argparse.ArgumentParser(
        description="Benchmark client-observed Redis request latency."
    )
    parser.add_argument("--host", default="127.0.0.1", help="server host (default: 127.0.0.1)")
    parser.add_argument("--port", type=int, default=6379, help="server port (default: 6379)")
    parser.add_argument(
        "--requests", type=int, default=10_000,
        help="total requests across all clients (default: 10000)",
    )
    parser.add_argument(
        "--clients", type=int, default=4,
        help="simultaneous client connections (default: 4)",
    )
    parser.add_argument(
        "--mode", choices=("get", "set", "mixed"), default="get",
        help="GET-only, SET-only, or alternating SET/GET workload (default: get)",
    )
    parser.add_argument(
        "--value-size", type=int, default=128,
        help="SET value size in bytes (default: 128)",
    )
    parser.add_argument(
        "--timeout", type=float, default=10,
        help="socket timeout in seconds (default: 10)",
    )
    args = parser.parse_args()

    if (
        args.requests <= 0
        or args.clients <= 0
        or args.value_size < 0
        or args.timeout <= 0
    ):
        parser.error(
            "--requests and --clients must be positive; "
            "--value-size cannot be negative; --timeout must be positive"
        )

    per_client = [
        args.requests // args.clients + (1 if i < args.requests % args.clients else 0)
        for i in range(args.clients)
    ]
    sockets = []
    try:
        for _ in range(args.clients):
            sock = socket.create_connection((args.host, args.port), timeout=args.timeout)
            sock.settimeout(args.timeout)
            sockets.append(sock)

        barrier = threading.Barrier(args.clients + 1)
        value = b"x" * args.value_size
        with concurrent.futures.ThreadPoolExecutor(max_workers=args.clients) as pool:
            futures = [
                pool.submit(
                    run_worker, sock, worker_id, per_client[worker_id],
                    args.mode, value, barrier,
                )
                for worker_id, sock in enumerate(sockets)
            ]
            start = time.perf_counter()
            barrier.wait()
            results = [future.result() for future in futures]
        elapsed = time.perf_counter() - start

        samples = sorted(sample for result in results for sample in result)
        print(f"Target:       {args.host}:{args.port}")
        print(f"Mode:         {args.mode}")
        print(f"Requests:     {len(samples)}")
        print(f"Clients:      {args.clients}")
        print(f"Elapsed:      {elapsed:.3f} s")
        print(f"Throughput:   {len(samples) / elapsed:,.0f} requests/s")
        print(f"Mean latency: {statistics.mean(samples) / 1_000:,.2f} us")
        for p in (50, 95, 99):
            print(f"P{p} latency:      {percentile(samples, p):,.2f} us")
        print(f"Max latency:  {samples[-1] / 1_000:,.2f} us")
        if args.mode in ("set", "mixed"):
            print("Note: SET requests append to appendonly.aof.")
    except (
        OSError,
        RuntimeError,
        ValueError,
        struct.error,
        concurrent.futures.BrokenExecutor,
    ) as error:
        parser.exit(1, f"Benchmark failed: {error}\n")
    finally:
        for sock in sockets:
            sock.close()


if __name__ == "__main__":
    main()
