# yap
Syslog daemon for BlueyOS

---

> ## ⚠️ IMPORTANT DISCLAIMER
>
> **This is a VIBE CODED, AI-GENERATED RESEARCH PROJECT.**
>
> yap was created as a component of [BlueyOS](https://github.com/nzmacgeek/biscuits),
> an experimental Linux-like OS built by AI models.
>
> **DO NOT USE THIS IN PRODUCTION.**

---

## What is yap?

**yap** is a standards-compliant syslog daemon for BlueyOS, named after Bluey's
irrepressible habit of chatting about everything in her day. It receives syslog
messages from the system and applications, and writes them to a log file.

yap follows [RFC 3164](https://datatracker.ietf.org/doc/html/rfc3164) — the
BSD syslog protocol — for both receiving and forwarding messages.

### Features

| Feature | Description |
|---------|-------------|
| `/dev/log` socket | Receives syslog messages from local applications |
| UDP port 514 | Optionally receives RFC 3164 messages from other hosts |
| Remote forwarding | Forwards messages to a remote syslog sink (UDP) |
| Log rotation | `yap-rotate` rotates logs by max file size or max age |
| claw integration | Ships with service units for claw (BlueyOS init system) |
| Configuration | `/etc/yap.yml` — YAML configuration file |

---

## Character mapping

| Component     | Character | Quote                                |
|---------------|-----------|--------------------------------------|
| `yap`         | Bluey     | "Let me tell you about my day!"      |
| `yap-rotate`  | Chilli    | "Time to tidy up the backyard!"      |

---

## Prerequisites

- `gcc` with `-m32` support (`gcc-multilib` on Debian/Ubuntu)
- `binutils` (`ld`, `ar`)
- `git`
- A musl-blueyos sysroot for i386 — see _Building musl-blueyos_ below

```bash
# Debian/Ubuntu
sudo apt-get install -y gcc-multilib binutils git
```

---

## Building

### 1. musl-blueyos sysroot

yap is built against [musl-blueyos](https://github.com/nzmacgeek/musl-blueyos),
a BlueyOS-flavoured fork of musl libc.

**On a BlueyOS build host** the sysroot is already installed at
`/opt/blueyos-sysroot` — the Makefile detects this automatically.

**On a fresh host** run the helper script to clone and build it:

```bash
make musl                        # clones musl-blueyos, installs into build/musl/
# or with a custom prefix:
make musl MUSL_PREFIX=/opt/blueyos-sysroot
```

### 2. Build yap

```bash
make                                        # static i386 ELF (default)
make static                                 # same, explicit
make dynamic                                # dynamically linked i386 ELF
make DEBUG=1                                # debug build (-g -O0)
make MUSL_PREFIX=/opt/blueyos-sysroot       # explicit sysroot path
make YAP_BLUEYOS_COMPAT=0                   # standard UNIX datagram / AF_INET build
make MUSL_PREFIX=/opt/blueyos-sysroot/usr install SYSROOT=/mnt/blueyos
make clean                                  # remove build/
```

Output files:

| Target              | Output                       | Description              |
|---------------------|------------------------------|--------------------------|
| `make` / `static`   | `build/yap`                  | static i386 ELF (musl)   |
| `make` / `static`   | `build/yap-rotate`           | static i386 ELF (musl)   |
| `make dynamic`      | `build/yap-dynamic`          | dynamic i386 ELF (musl)  |
| `make dynamic`      | `build/yap-rotate-dynamic`   | dynamic i386 ELF (musl)  |

### BlueyOS compatibility mode

By default, yap builds with `YAP_BLUEYOS_COMPAT=1`.

This mode matches the current BlueyOS kernel socket ABI rather than the
traditional Linux syslog ABI:

- `/dev/log` is created as `AF_UNIX` + `SOCK_STREAM`
- local stream clients are accepted and newline-delimited syslog records are parsed
- UDP listen/forwarding code is compiled in but feature-gated off at runtime

This is a short-term compatibility mode for current BlueyOS. It does **not**
make musl-blueyos `syslog(3)` fully compatible yet, because musl currently
opens `/dev/log` as `AF_UNIX` + `SOCK_DGRAM`.

Use `YAP_BLUEYOS_COMPAT=0` if you want the standard local datagram socket and
AF_INET UDP behavior on a host that actually supports them.

---

## Configuration

Configuration is read from `/etc/yap.yml` at startup, and reloaded on `SIGHUP`.

```yaml
# /etc/yap.yml
log_file: /var/log/system.log   # output log file
socket: /dev/log                # UNIX socket path
listen_udp: false               # listen on UDP port 514?
udp_port: 514                   # UDP port (if listen_udp is true)

forward:
  enabled: false                # forward to remote syslog sink?
  host: ""                      # remote host
  port: 514                     # remote port

rotation:
  enabled: true
  max_size: 10485760            # 10 MiB — rotate when log exceeds this
  max_age: 604800               # 7 days — rotate when log is older than this
  keep_count: 5                 # number of rotated files to retain
```

---

## Signals

| Signal    | Action                                     |
|-----------|--------------------------------------------|
| `SIGHUP`  | Reload `/etc/yap.yml` and reopen log file  |
| `SIGUSR1` | Reopen log file (sent by `yap-rotate`)     |
| `SIGTERM` | Graceful shutdown                          |

---

## Log format

Each syslog message is written in the following format:

```
MMM DD HH:MM:SS hostname tag[pid]: <facility.severity> message
```

Example:

```
Jan  1 12:34:56 blueyos kernel[1]: <kern.info> BlueyOS booting up!
Jan  1 12:34:57 blueyos sshd[1234]: <auth.notice> Accepted publickey for bandit
```

---

## Log rotation

`yap-rotate` is a companion utility that rotates the syslog log file when it
exceeds a configured size or age. It is designed to be called periodically by
claw's timer service.

```
Usage: yap-rotate [-f] [-c config]
  -f        force rotation regardless of thresholds
  -c FILE   configuration file (default: /etc/yap.yml)
  -h        show this help
```

### How rotation works

1. `yap-rotate` reads `/etc/yap.yml` and checks if rotation is needed.
2. If the log exceeds `max_size` bytes or `max_age` seconds, it renames the
   current log to a timestamped archive: `system.log.20240101T120000.0`
3. It sends `SIGUSR1` to `yap` so it opens a fresh log file.
4. It removes old rotated files beyond `keep_count`.

---

## Integration with BlueyOS (claw)

yap ships two claw service unit files:

| File | Description |
|------|-------------|
| `/etc/claw/services.d/yap.service.yml` | Main syslog daemon — starts before `claw-basic.target` |
| `/etc/claw/services.d/yap-rotate.service.yml` | Log rotation timer — runs hourly after yap is up |

### Boot sequence

yap starts after `claw-rootfs.target` (filesystem is read-write) and before
`claw-basic.target`, ensuring the `/dev/log` socket is available to all
services that follow.

```
claw-rootfs.target → yap → claw-basic.target → (rest of services)
```

### Claw integration detail

yap uses `type: simple` with `-n` (no-daemonize) so claw can supervise it
directly. This means claw captures its stdout/stderr and tracks its PID.

### Current socket ABI caveat

Current BlueyOS exposes `AF_UNIX` stream sockets plus a custom `AF_NETCTL`
family, but not `AF_UNIX` datagram sockets or `AF_INET` userland sockets.
That is why yap's compatibility mode uses a stream listener for `/dev/log`
and disables UDP receive/forwarding.

---

## musl-blueyos syslog behavior

musl-blueyos currently follows the traditional syslog client model:

- `openlog()` opens `socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0)`
- it `connect()`s that socket to `/dev/log`
- `syslog()` sends complete RFC 3164 messages with `send()`

That means the current BlueyOS kernel, musl-blueyos, and yap do not yet form a
fully compatible local syslog stack. yap's stream-mode listener is only a
temporary server-side adaptation.

## Minimum kernel work for proper AF_UNIX datagram support

To support the normal `/dev/log` syslog path without compatibility mode, the
minimum BlueyOS kernel work is:

1. Accept `AF_UNIX` + `SOCK_DGRAM` in the socket creation path instead of only
  `SOCK_STREAM`.
2. Add datagram socket state that binds directly to a pathname without
  requiring `listen()` / `accept()`.
3. Implement datagram `connect()` so a client can associate its socket with a
  bound pathname like `/dev/log`.
4. Preserve message boundaries for local delivery so one `send()` becomes one
  receive record at the server side.
5. Implement the user-visible receive/send path for connected datagrams,
  ideally covering `send`, `recv`, `sendto`, and `recvfrom` semantics.
6. Keep readiness and error reporting aligned with the existing syscall layer
  so unsupported families return `EAFNOSUPPORT` and unsupported types return
  `EPROTONOSUPPORT`.

AF_INET UDP syslog is separate follow-on work. The current compatibility mode
only addresses the local `/dev/log` path.

---

## Packaging (dimsim)

yap ships as a [dimsim](https://github.com/nzmacgeek/dimsim) `.dpk` package.

### Package contents

| Installed path | Description |
|----------------|-------------|
| `/sbin/yap` | The syslog daemon binary (static i386 ELF) |
| `/sbin/yap-rotate` | The log rotation utility (static i386 ELF) |
| `/etc/yap.yml` | Default configuration |
| `/etc/claw/services.d/yap.service.yml` | Claw service — syslog daemon |
| `/etc/claw/services.d/yap-rotate.service.yml` | Claw timer service — log rotation |

### Building the package

Requires `dpkbuild` from [dimsim](https://github.com/nzmacgeek/dimsim) on your `PATH`.

```bash
make package
```

This builds the static i386 ELF binaries, stages them under `pkg/payload/sbin/`,
and invokes `dpkbuild build pkg/` to produce `yap-<version>-i386.dpk`.

### Installing into an offline sysroot

If you want to copy yap directly into a mounted rootfs without creating a
`.dpk`, use `make install` with an explicit sysroot:

```bash
mount -o loop blueyos.img /mnt/blueyos
make MUSL_PREFIX=/opt/blueyos-sysroot/usr install SYSROOT=/mnt/blueyos
umount /mnt/blueyos
```

This installs:

- `/sbin/yap`
- `/sbin/yap-rotate`
- `/etc/yap.yml`
- `/etc/claw/services.d/yap.service.yml`
- `/etc/claw/services.d/yap-rotate.service.yml`

It does not build a dimsim package or run package lifecycle scripts.

### Installing via dimsim package

```bash
# Mount the target rootfs
mount -o loop blueyos.img /mnt/blueyos

# Install yap (no network needed — pass the local .dpk directly)
dimsim --root /mnt/blueyos install ./yap-<version>-i386.dpk

# Unmount and boot
umount /mnt/blueyos
```

On first boot, claw loads the service unit files placed under `/etc/claw/`
and starts yap before `claw-basic.target`. All subsequent services that log
via syslog will have `/dev/log` available.

---

## Usage

```
Usage: yap [-n] [-c config]
  -n        do not daemonize (run in foreground)
  -c FILE   configuration file (default: /etc/yap.yml)
  -h        show this help
```

Typically claw starts yap via its service unit (using `-n` for direct
supervision). To run manually in the foreground for testing:

```bash
/sbin/yap -n -c /etc/yap.yml
```

---

Bluey and all related characters are trademarks of Ludo Studio Pty Ltd,
licensed by BBC Studios. This is an unofficial fan/research project with
no affiliation to Ludo Studio or the BBC.
