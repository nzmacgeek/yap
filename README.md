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
make clean                                  # remove build/
```

Output files:

| Target              | Output                       | Description              |
|---------------------|------------------------------|--------------------------|
| `make` / `static`   | `build/yap`                  | static i386 ELF (musl)   |
| `make` / `static`   | `build/yap-rotate`           | static i386 ELF (musl)   |
| `make dynamic`      | `build/yap-dynamic`          | dynamic i386 ELF (musl)  |
| `make dynamic`      | `build/yap-rotate-dynamic`   | dynamic i386 ELF (musl)  |

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
