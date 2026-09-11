# CareDispatch

CareDispatch is a compact operating-systems project written in C. It models a
local alarm dispatcher for home-care services: clients send alarms over TCP,
the server prioritizes urgent events, worker threads process them concurrently,
and a crash-resistant journal plus memory-mapped statistics preserve history.

## Build and run

Linux or WSL is recommended.

```bash
make
./caredispatch
```

In another terminal:

```bash
./send_alarm FALL Anna "Fell in the bathroom"
./send_alarm MEDICINE Fatima "Missed evening medicine"
./send_alarm SERVICE Erik "Needs help with dishes"
```

Stop the server with `Ctrl+C`. It drains the queue and prints a report. Run the
complete integration test with `make test` and reset generated data with
`make clean`.

## Course concepts demonstrated

| Course concept | Where it appears |
| --- | --- |
| Processes and `fork()` | A child process produces the final report |
| Pipe and file descriptors | Parent receives the child's report through a pipe |
| POSIX threads | Client handlers and three worker threads |
| Mutex | Protects queue, statistics, IDs, and journal writes |
| Condition variable | Workers sleep while the alarm queue is empty |
| Counting semaphore | Limits the bounded queue to 16 alarms |
| Producer-consumer | Socket clients produce; workers consume |
| Sockets | TCP communication on localhost port 5050 |
| Dynamic memory and pointers | Each alarm/client job uses `malloc` and `free` |
| Persistent storage | Append-only journal uses `fopen`, `fsync`, and `close` |
| Virtual memory / `mmap` | Statistics are mapped with `MAP_SHARED` |
| Signals | `SIGINT` and `SIGTERM` trigger an orderly shutdown |
| Scheduling idea | Fall alarms have priority over medicine and service alarms |

## Architecture

1. A client connects and sends `TYPE|NAME|MESSAGE`.
2. A detached client thread parses and allocates the alarm.
3. The bounded priority queue applies backpressure when full.
4. Worker threads compete for queued work without race conditions.
5. Every state transition is journaled and flushed to disk.
6. At shutdown, a forked child builds a report and sends it through a pipe.

The server keeps all shared state inside an `App` structure and passes pointers
to threads; it intentionally uses no mutable global variables.

## Important limitations

This is an educational local prototype, not production medical software. It has
no authentication, encryption, database, retry protocol, or formal guarantee
that a detached client thread has ended before shutdown. Those are natural next
iterations after the OS mechanisms are understood.
