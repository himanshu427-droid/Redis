
#!/usr/bin/env python3

import argparse
import concurrent.futures
import socket
import statistics
import threading
import time


def encode_command(*args):
    parts = [f"*{len(args)}\r\n".encode()]

    for arg in args:
        if isinstance(arg, bytes):
            value = arg
        else:
            value = str(arg).encode()

        parts.append(f"${len(value)}\r\n".encode())
        parts.append(value)
        parts.append(b"\r\n")

    return b"".join(parts)


def read_line(sock):
    data = bytearray()

    while not data.endswith(b"\r\n"):
        chunk = sock.recv(1)

        if not chunk:
            raise ConnectionError("Server closed connection")

        data.extend(chunk)

    return bytes(data[:-2])


def read_exact(sock, size):
    data = bytearray()

    while len(data) < size:
        chunk = sock.recv(size - len(data))

        if not chunk:
            raise ConnectionError("Server closed connection")

        data.extend(chunk)

    return bytes(data)


def read_response(sock):
    prefix = read_exact(sock, 1)

    if prefix == b"+":
        return read_line(sock)

    if prefix == b"-":
        raise RuntimeError(
            "Redis error: " + read_line(sock).decode(errors="replace")
        )

    if prefix == b":":
        return int(read_line(sock))

    if prefix == b"$":
        size = int(read_line(sock))

        if size == -1:
            return None

        data = read_exact(sock, size)
        read_exact(sock, 2)  # CRLF

        return data

    if prefix == b"*":
        count = int(read_line(sock))

        if count == -1:
            return None

        return [read_response(sock) for _ in range(count)]

    if prefix == b"_":
        read_line(sock)
        return None

    raise ValueError(f"Unknown RESP response prefix: {prefix!r}")


def request(sock, *args):
    sock.sendall(encode_command(*args))
    return read_response(sock)


def run_worker(sock, worker_id, request_count, mode,
               value, start_barrier):

    latencies = []
    key = f"benchmark:{worker_id}"

    start_barrier.wait()

    for index in range(request_count):

        if mode == "set":
            command = ("SET", f"{key}:{index}", value)

        elif mode == "get":
            command = ("GET", key)

        elif mode == "mixed" and index % 2 == 0:
            command = ("SET", key, value)

        else:
            command = ("GET", key)

        before = time.perf_counter_ns()

        response = request(sock, *command)

        elapsed = time.perf_counter_ns() - before

        if command[0] == "SET" and response != b"OK":
            raise RuntimeError(
                f"Worker {worker_id}: unexpected SET response {response!r}"
            )

        latencies.append(elapsed)

    return latencies


def percentile(sorted_samples, percent):
    index = max(
        0,
        (len(sorted_samples) * percent + 99) // 100 - 1
    )

    return sorted_samples[index] / 1000


def main():

    parser = argparse.ArgumentParser(
        description="Benchmark Redis using RESP2."
    )

    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=6380)

    parser.add_argument("--requests", type=int, default=10000)
    parser.add_argument("--clients", type=int, default=4)

    parser.add_argument(
        "--mode",
        choices=("get", "set", "mixed"),
        default="mixed"
    )

    parser.add_argument("--value-size", type=int, default=128)
    parser.add_argument("--timeout", type=float, default=10)

    args = parser.parse_args()

    if (
        args.requests <= 0
        or args.clients <= 0
        or args.value_size < 0
        or args.timeout <= 0
    ):
        parser.error("Invalid benchmark arguments")

    per_client = [
        args.requests // args.clients
        + (1 if i < args.requests % args.clients else 0)
        for i in range(args.clients)
    ]

    sockets = []

    try:

        for _ in range(args.clients):

            sock = socket.create_connection(
                (args.host, args.port),
                timeout=args.timeout
            )

            sock.settimeout(args.timeout)
            sockets.append(sock)

        barrier = threading.Barrier(args.clients + 1)

        value = b"x" * args.value_size

        with concurrent.futures.ThreadPoolExecutor(
            max_workers=args.clients
        ) as pool:

            futures = [
                pool.submit(
                    run_worker,
                    sock,
                    worker_id,
                    per_client[worker_id],
                    args.mode,
                    value,
                    barrier
                )
                for worker_id, sock in enumerate(sockets)
            ]

            start = time.perf_counter()

            barrier.wait()

            results = [
                future.result()
                for future in futures
            ]

        elapsed = time.perf_counter() - start

        samples = sorted(
            sample
            for result in results
            for sample in result
        )

        print(f"Target:       {args.host}:{args.port}")
        print(f"Protocol:     RESP2")
        print(f"Mode:         {args.mode}")
        print(f"Requests:     {len(samples)}")
        print(f"Clients:      {args.clients}")
        print(f"Value size:   {args.value_size} bytes")
        print(f"Elapsed:      {elapsed:.3f} s")

        print(
            f"Throughput:   {len(samples) / elapsed:,.0f} requests/s"
        )

        print(
            f"Mean latency: {statistics.mean(samples) / 1000:,.2f} us"
        )

        for p in (50, 95, 99):
            print(
                f"P{p} latency:      {percentile(samples, p):,.2f} us"
            )

        print(
            f"Max latency:  {samples[-1] / 1000:,.2f} us"
        )

    except (
        OSError,
        RuntimeError,
        ValueError,
        concurrent.futures.BrokenExecutor
    ) as error:

        parser.exit(1, f"Benchmark failed: {error}\n")

    finally:

        for sock in sockets:
            sock.close()


if __name__ == "__main__":
    main()
