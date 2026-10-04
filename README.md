# QNX CNC Driver

A QNX resource manager that exposes a CNC lathe as two files, talking to the machine over OPC UA.

## Overview

The driver is a single QNX process. From the application side it is a resource manager; from the machine side it is an OPC UA client. Two paths are exposed:

- **`/dev/cnc/plant`** — reading returns the latest machine state as one `cnc_plant_t` struct: spindle, feed, tool, vibration, production, auxiliary systems, and machine information. Values come from a single reading on the machine, so they are consistent with each other. Reads are answered instantly from a cached snapshot; the driver polls the machine in the background.
- **`/dev/cnc/methods`** — writing one `cnc_cmd_t` runs a method on the machine: `EmergencyStop`, `ResetProductionCounters`, or `ChangeTool`. The call blocks until the machine answers, or fails with an `errno`.

The two OPC UA sessions are owned by separate threads inside the driver. One polls the machine on a fixed cadence, one runs commands as they arrive. Pool threads that serve applications never wait on the network.

## Requirements

- QNX SDP 8.0
- open62541 (see [Dependencies](#dependencies))
- A QNX target or VM to run the driver
- An OPC UA server exposing the machine, reachable over the network

## Dependencies

The driver uses the open62541 single-file amalgamation. Download the latest 1.5.x release from:

- https://github.com/open62541/open62541/releases

You need two files:

- `open62541.c`
- `open62541.h`

Place both files in `src/`. The Makefile picks them up automatically along with the driver source.

open62541 is licensed under MPL 2.0. See the project page for details.

## Build

The Makefile is the standard Momentics template. It expects the QNX build environment to be set up (`QNX_TARGET` and `QNX_HOST` exported).

```sh
make
```

By default it builds for `x86_64` in debug mode. To build for another target or profile:

```sh
make PLATFORM=armv7le BUILD_PROFILE=release
```

The output binary is written to `build/<platform>-<profile>/MyDriver`.

To clean:

```sh
make clean
```

## Run

On the QNX target:

```sh
./MyDriver -U 100:100 opc.tcp://192.168.1.50:4840/freeopcua/server/
```

`-U uid:gid` drops root after the driver has registered its paths under `/dev`. The URL is the OPC UA endpoint of the machine.

The driver logs to stderr:

```
2026-10-04 21:13:09.482 INFO  serving /dev/cnc/plant and /dev/cnc/methods
2026-10-04 21:13:09.994 INFO  read session up (opc.tcp://192.168.1.50:4840/freeopcua/server/)
```

The write session is created on the first command.

To stop it cleanly:

```sh
slay MyDriver
```

The driver drains the command queue, closes both OPC UA sessions, and exits.

## Usage

### Reading the machine

```c
#include "opcua_cnc_map.h"

cnc_plant_t p;
int fd = open(CNC_PATH_PLANT, O_RDONLY);
pread(fd, &p, sizeof p, 0);
printf("%.0f rpm, tool %d\n", p.spindle.speed_rpm, (int)p.tool.number);
```

A plain `read()` at offset 0 returns the snapshot and moves the file position past it. A second `read()` returns 0 (end of file). To poll, keep the file open and use `pread()` at offset 0 — it always returns the latest snapshot without moving the file position.

If the link to the machine is down, the driver keeps the last known values and clears the `connected` flag in `p.hdr.connected`. The application can still read them, but should check that flag.

### Sending a command

```c
cnc_cmd_t cmd = { CNC_CHANGE_TOOL, 4 };
int fd = open(CNC_PATH_METHODS, O_WRONLY);
if (write(fd, &cmd, sizeof cmd) != sizeof cmd)
    perror("ChangeTool");
```

`write()` blocks until the machine has executed the method, then returns `sizeof(cnc_cmd_t)`. On failure it returns `-1` and sets `errno`.

### Errors

| `errno` | meaning |
|---|---|
| `EBUSY` | the command queue is full; retry later |
| `EIO` | no link to the machine |
| `ETIMEDOUT` | the machine did not answer in time |
| `EACCES` | the machine refused the method |
| `EINVAL` | malformed command |
| `EINTR` | the caller was interrupted while waiting (see below) |
| `ECANCELED` | the driver is shutting down |

`EINTR` means the caller was interrupted by a signal while blocked in `write()`. The driver cannot tell whether the machine has already started executing the method. For idempotent methods (`EmergencyStop`, `ResetProductionCounters`) retrying is safe. For `ChangeTool`, an application that cares should re-read `/dev/cnc/plant` and check `tool.number` before deciding.

## Repository layout

```
src/
  MyDriver.c            the resource manager: paths, handlers, queue, threads, shutdown
  opcua_client.c        the OPC UA client: two sessions, snapshot, method calls
  opcua_cnc_map.h       the public interface: paths, structs, errno meanings
  opcua_client.h        internal interface between the two .c files
  cnc_log.h             timestamped log lines on stderr
Makefile                standard Momentics build
```

## Interface

`opcua_cnc_map.h` is the full contract. It declares:

- the two paths (`CNC_PATH_PLANT`, `CNC_PATH_METHODS`)
- `cnc_plant_t` — what a read on `/dev/cnc/plant` returns
- `cnc_cmd_t` — what a write on `/dev/cnc/methods` takes
- the method enum (`cnc_method_t`)
- the machine state and tool state enums
- the `errno` values that `read()` and `write()` can return

Applications include this header and nothing else.

## Design notes

- The driver polls the machine over OPC UA on a fixed cadence (default 500 ms) and stores the result in a snapshot. Reads are answered from the snapshot, not from the network.
- Commands are queued. The pool thread that receives a `write()` does not wait for the machine; it pushes the command and the writer's `rcvid` onto the queue and returns. The write thread runs the command and replies to the writer when the machine is done.
- Two OPC UA sessions, each owned by exactly one thread. This avoids the open62541 thread-safety issue that would otherwise require serialising the periodic Read against every method call.
- On shutdown, the driver stops accepting commands, drains the queue, and closes both sessions cleanly so the server sees normal disconnects.

## Status

Built and tested against QNX SDP 8.0 on x86_64, with an OPC UA server on a Raspberry Pi 4.

## License

MIT — see [LICENSE](LICENSE).

This project does not include any QNX source code. It uses QNX headers and libraries under the terms of the QNX license held by the user.