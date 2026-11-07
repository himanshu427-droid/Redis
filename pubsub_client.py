import socket
import struct
import sys

def readn(sock, size):
    data = b""
    while len(data) < size:
        part = sock.recv(size - len(data))
        if not part:
            raise ConnectionError("Server closed the connection")
        data += part
    return data

def read_value(data, pos=0):
    tag = data[pos]
    pos += 1

    if tag == 0:
        return None, pos
    if tag == 1:
        code, size = struct.unpack_from("<II", data, pos)
        pos += 8
        return f"ERROR {code}: {data[pos:pos + size].decode(errors='replace')}", pos + size
    if tag == 2:
        size = struct.unpack_from("<I", data, pos)[0]
        pos += 4
        return data[pos:pos + size].decode(errors="replace"), pos + size
    if tag == 3:
        return struct.unpack_from("<q", data, pos)[0], pos + 8
    if tag == 4:
        return struct.unpack_from("<d", data, pos)[0], pos + 8
    if tag == 5:
        count = struct.unpack_from("<I", data, pos)[0]
        pos += 4
        items = []
        for _ in range(count):
            item, pos = read_value(data, pos)
            items.append(item)
        return items, pos
    raise ValueError(f"Unknown response type: {tag}")

def command(sock, *args):
    body = struct.pack("<I", len(args))
    for arg in args:
        value = arg.encode()
        body += struct.pack("<I", len(value)) + value

    sock.sendall(struct.pack("<I", len(body)) + body)
    size = struct.unpack("<I", readn(sock, 4))[0]
    return read_value(readn(sock, size))[0]

if len(sys.argv) < 3 or sys.argv[1] not in ("sub", "pub"):
    print('Usage: python3 pubsub_client.py sub CHANNEL')
    print('   or: python3 pubsub_client.py pub CHANNEL MESSAGE')
    sys.exit(1)

mode, channel = sys.argv[1:3]

with socket.create_connection(("127.0.0.1", 6379) ) as sock:
    if mode == "sub":
        print("Subscribed:", command(sock, "subscribe", channel))
        print("Listening for messages; press Ctrl+C to quit.")
        while True:
            size = struct.unpack("<I", readn(sock, 4))[0]
            print("Received:", read_value(readn(sock, size))[0], flush=True)
    else:
        if len(sys.argv) < 4:
            print("Publisher requires a message.")
            sys.exit(1)
        message = " ".join(sys.argv[3:])
        print("Publish subscriber count:", command(sock, "publish", channel, message))