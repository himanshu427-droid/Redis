# Redis

A Redis-inspired in-memory key-value server written in C++.

## Features

* TCP client-server communication
* In-memory key-value storage
* Key expiration (TTL)
* Sorted sets
* Append-only persistence for string set/delete/expire commands
* Pub/sub channels with `subscribe`, `unsubscribe`, and `publish`
* Transactions with `multi`, `exec`, and `discard`
* Custom hash table
* AVL tree and heap implementations
* Thread pool for background tasks

The server stores its append-only log in `appendonly.aof` in the working
directory and replays it on startup.

## Build

```bash
g++ $(ls *.cpp | grep -v "client.cpp") -o redis_server
```

## Run

```bash
./redis_server
```

Connect using the included client, a TCP client, or Redis CLI

## Structure

```text
server.cpp          Server and networking
client.cpp          Client
hashtable.cpp       Key-value storage
avl.cpp             AVL tree
zset.cpp            Sorted sets
heap.cpp            TTL management
thread_pool.cpp     Worker thread pool
```
