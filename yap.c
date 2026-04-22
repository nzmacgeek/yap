/* Feature-test macros — must appear before any system headers */
#define _POSIX_C_SOURCE 200809L

/* Compile-time debug tracing — always on for BlueyOS diagnostic builds.
 * Writes to /dev/console directly so output appears on the serial console
 * even when stderr has been redirected to a log file by the supervisor. */
static void yap_dbg_write(const char *fmt, ...) __attribute__((format(printf,1,2)));
#define YAP_DBG(fmt, ...) \
    yap_dbg_write("[yap dbg %s:%d] " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__)

/*
 * yap - syslog daemon for BlueyOS
 * "Let me tell you about my day!" - Bluey Heeler
 *
 * yap is a standards-compliant syslog daemon for BlueyOS. It listens on
 * a local input endpoint for syslog messages from local applications, optionally accepts
 * RFC 3164 messages on UDP port 514, writes them to /var/log/system.log
 * (configurable), and can forward them to a remote syslog sink.
 *
 * Configuration: /etc/yap.yml
 * PID file:      /var/run/yap.pid
 *
 * Signals:
 *   SIGHUP  — reload configuration and reopen log file
 *   SIGUSR1 — reopen log file (used by yap-rotate after rotation)
 *   SIGTERM — graceful shutdown
 *
 * Built as i386 ELF, statically linked against musl-blueyos by default.
 *
 * Bluey and all related characters are trademarks of Ludo Studio Pty Ltd,
 * licensed by BBC Studios. This is an unofficial fan/research project with
 * no affiliation to Ludo Studio or the BBC.
 *
 * ⚠️  VIBE CODED RESEARCH PROJECT — NOT FOR PRODUCTION USE ⚠️
 */

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

/* Implementation of yap_dbg_write (forward-declared above with YAP_DBG macro) */
static void yap_dbg_write(const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    if (n >= (int)sizeof(buf)) n = (int)sizeof(buf) - 1;
    int cfd = open("/dev/console", O_WRONLY);
    if (cfd >= 0) { write(cfd, buf, (size_t)n); close(cfd); }
    write(STDERR_FILENO, buf, (size_t)n);
}

/* -------------------------------------------------------------------------
 * Constants
 * ---------------------------------------------------------------------- */

#ifndef YAP_VERSION
#define YAP_VERSION     "0.2.0"
#endif
#define YAP_CONFIG      "/etc/yap.yml"
#define YAP_PIDFILE     "/var/run/yap.pid"
#define YAP_SOCKET_PATH "/run/log/yap.inbox"
#define YAP_READYFILE   "/run/log/yap.ready"
#define YAP_LOGLOCK     "/run/log/yap.system.lock"
#define YAP_LOGFILE     "/var/log/system.log"
#define YAP_UDP_PORT    514

#define MAX_MSG     8192   /* maximum incoming syslog message size */
#define MAX_PATH    512    /* maximum path length */
#define MAX_HOST    256    /* maximum hostname length */
#define MAX_LINE    1024   /* maximum line length in config/output */
#define MAX_TAG     128    /* maximum syslog tag length */

/*
 * BlueyOS compatibility mode targets the current biscuits userspace/kernel ABI.
 * AF_UNIX readiness and receive behavior are still being stabilized there, so
 * local syslog uses a simple spool directory while UDP features stay disabled.
 */
#ifndef YAP_BLUEYOS_COMPAT
#define YAP_BLUEYOS_COMPAT 1
#endif

#if YAP_BLUEYOS_COMPAT
#define YAP_LOCAL_SOCKET_LABEL "spool directory"
#define YAP_HAVE_INET_SOCKETS  0
#else
#define YAP_LOCAL_SOCKET_TYPE  SOCK_DGRAM
#define YAP_LOCAL_SOCKET_LABEL "AF_UNIX/SOCK_DGRAM"
#define YAP_HAVE_INET_SOCKETS  1
#endif

/* Syslog facility codes (RFC 3164) */
#define FAC_KERN    0
#define FAC_USER    1
#define FAC_MAIL    2
#define FAC_DAEMON  3
#define FAC_AUTH    4
#define FAC_SYSLOG  5
#define FAC_LPR     6
#define FAC_NEWS    7
#define FAC_UUCP    8
#define FAC_CRON    9
#define FAC_LOCAL0  16
#define FAC_LOCAL1  17
#define FAC_LOCAL2  18
#define FAC_LOCAL3  19
#define FAC_LOCAL4  20
#define FAC_LOCAL5  21
#define FAC_LOCAL6  22
#define FAC_LOCAL7  23

/* Syslog severity codes (RFC 3164) */
#define SEV_EMERG   0
#define SEV_ALERT   1
#define SEV_CRIT    2
#define SEV_ERR     3
#define SEV_WARNING 4
#define SEV_NOTICE  5
#define SEV_INFO    6
#define SEV_DEBUG   7

/* -------------------------------------------------------------------------
 * Configuration structure
 * ---------------------------------------------------------------------- */

struct yap_config {
    char log_file[MAX_PATH];   /* output log file path */
    char socket_path[MAX_PATH];/* local input path */
    int  listen_udp;           /* 1 = listen on UDP 514 */
    int  udp_port;             /* UDP port to listen on */

    /* Remote forwarding */
    int  forward_enabled;      /* 1 = forward to remote syslog */
    char forward_host[MAX_HOST];
    int  forward_port;
};

/* -------------------------------------------------------------------------
 * Global state
 * ---------------------------------------------------------------------- */

static struct yap_config g_cfg;
static int  g_log_fd   = -1;   /* output log file fd */
static int  g_unix_fd  = -1;   /* local input handle */
static int  g_udp_fd   = -1;   /* UDP socket fd */
static int  g_fwd_fd   = -1;   /* forwarding socket fd */
static char g_hostname[MAX_HOST];
static char g_recv_buf[MAX_MSG];
static char g_write_buf[MAX_MSG + MAX_HOST + MAX_TAG + 128];
static int  g_ready_written = 0;

#if YAP_BLUEYOS_COMPAT
#define YAP_COMPAT_SPOOL_MODE   01777
#define YAP_COMPAT_SUCCESS      0
#define YAP_MAX_LOCAL_SOURCES   256

struct local_source {
    char name[MAX_PATH];
    off_t offset;
    char line_buf[MAX_MSG];
    size_t line_len;
    struct local_source *next;
};

static struct local_source *g_local_sources = NULL;
static size_t g_local_source_count = 0;
static int g_local_source_limit_warned = 0;
#endif

/* Signal flags — set by signal handlers, checked in main loop */
static volatile sig_atomic_t g_flag_reload   = 0;
static volatile sig_atomic_t g_flag_reopen   = 0;
static volatile sig_atomic_t g_flag_shutdown = 0;

static void process_message(const char *buf, size_t len);
#if YAP_BLUEYOS_COMPAT
static void clear_local_sources(void);
#endif

static void write_log_bytes(const char *buf, size_t len)
{
    struct flock lock;
    size_t written = 0;

    if (g_log_fd < 0 || !buf || len == 0)
        return;

    memset(&lock, 0, sizeof(lock));
    lock.l_type = F_WRLCK;
    lock.l_whence = SEEK_SET;

    while (fcntl(g_log_fd, F_SETLKW, &lock) < 0) {
        if (errno == EINTR)
            continue;
        return;
    }

    if (lseek(g_log_fd, 0, SEEK_END) < 0) {
        lock.l_type = F_UNLCK;
        (void)fcntl(g_log_fd, F_SETLK, &lock);
        return;
    }

    while (written < len) {
        ssize_t n = write(g_log_fd, buf + written, len - written);
        if (n > 0) {
            written += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        break;
    }

    lock.l_type = F_UNLCK;
    (void)fcntl(g_log_fd, F_SETLK, &lock);
}

/* -------------------------------------------------------------------------
 * Signal handlers (async-signal-safe — only set flags)
 * ---------------------------------------------------------------------- */

static void handle_sighup(int sig)
{
    (void)sig;
    g_flag_reload = 1;
}

static void handle_sigusr1(int sig)
{
    (void)sig;
    g_flag_reopen = 1;
}

static void handle_sigterm(int sig)
{
    (void)sig;
    g_flag_shutdown = 1;
}

/* -------------------------------------------------------------------------
 * Internal logging (before log file is open or for daemon-level messages)
 * ---------------------------------------------------------------------- */

static void yap_log(const char *fmt, ...)
{
    va_list ap;
    char buf[MAX_LINE];
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char ts[64];

    strftime(ts, sizeof(ts), "%b %e %H:%M:%S", tm_info);
    snprintf(buf, sizeof(buf), "%s %s yap[%d]: ", ts, g_hostname, (int)getpid());

    size_t prefix_len = strlen(buf);
    va_start(ap, fmt);
    vsnprintf(buf + prefix_len, sizeof(buf) - prefix_len, fmt, ap);
    va_end(ap);

    size_t len = strlen(buf);
    if (len < sizeof(buf) - 2) {
        buf[len]     = '\n';
        buf[len + 1] = '\0';
    }

    /* In BlueyOS compatibility mode, keep startup chatter off system.log until ready. */
    if (g_log_fd >= 0 && (!YAP_BLUEYOS_COMPAT || g_ready_written)) {
        write_log_bytes(buf, strlen(buf));
    } else {
        fputs(buf, stderr);
    }
}

static void bstrncpy(char *dst, const char *src, size_t dst_size)
{
    size_t n;

    if (!dst || dst_size == 0)
        return;
    if (!src) {
        dst[0] = '\0';
        return;
    }

    n = strnlen(src, dst_size - 1);
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* -------------------------------------------------------------------------
 * Config loading — minimal YAML parser
 * Handles:
 *   key: value
 *   section:
 *     key: value
 * ---------------------------------------------------------------------- */

static void config_defaults(struct yap_config *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    strncpy(cfg->log_file,    YAP_LOGFILE,     sizeof(cfg->log_file) - 1);
    cfg->log_file[sizeof(cfg->log_file) - 1] = '\0';
    strncpy(cfg->socket_path, YAP_SOCKET_PATH, sizeof(cfg->socket_path) - 1);
    cfg->socket_path[sizeof(cfg->socket_path) - 1] = '\0';
    cfg->listen_udp      = 0;
    cfg->udp_port        = YAP_UDP_PORT;
    cfg->forward_enabled = 0;
    cfg->forward_port    = YAP_UDP_PORT;
}

/*
 * Trim leading and trailing whitespace in-place.
 * Returns pointer to the first non-space character.
 */
static char *trim(char *s)
{
    char *end;
    while (*s == ' ' || *s == '\t')
        s++;
    end = s + strlen(s);
    while (end > s && (*(end - 1) == ' ' || *(end - 1) == '\t' ||
                       *(end - 1) == '\r' || *(end - 1) == '\n'))
        end--;
    *end = '\0';
    return s;
}

static int config_load(const char *path, struct yap_config *cfg)
{
    FILE *f;
    char line[MAX_LINE];
    char section[64] = "";   /* current top-level section, or "" for root */

    config_defaults(cfg);

    f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "yap: cannot open config %s: %s\n", path, strerror(errno));
        return -1;
    }

    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        int indent = 0;

        /* Count leading spaces to determine nesting */
        while (*p == ' ') { indent++; p++; }

        /* Strip comments and blank lines */
        if (*p == '#' || *p == '\n' || *p == '\r' || *p == '\0')
            continue;

        /* Split on first ':' */
        char *colon = strchr(p, ':');
        if (!colon)
            continue;

        *colon = '\0';
        char *key = trim(p);
        char *val = trim(colon + 1);

        /* Detect section headers (indent == 0, value empty) */
        if (indent == 0 && *val == '\0') {
            strncpy(section, key, sizeof(section) - 1);
            section[sizeof(section) - 1] = '\0';
            continue;
        }

        /* Strip optional inline comments from value */
        char *hash = strchr(val, '#');
        if (hash) {
            /* Only strip if preceded by whitespace */
            if (hash > val && (*(hash - 1) == ' ' || *(hash - 1) == '\t')) {
                *hash = '\0';
                val = trim(val);
            }
        }

        /* Match key/value against known config options */
        if (indent == 0) {
            /* Root-level key */
            if (strcmp(key, "log_file") == 0) {
                strncpy(cfg->log_file, val, sizeof(cfg->log_file) - 1);
                cfg->log_file[sizeof(cfg->log_file) - 1] = '\0';
            } else if (strcmp(key, "socket") == 0) {
                strncpy(cfg->socket_path, val, sizeof(cfg->socket_path) - 1);
                cfg->socket_path[sizeof(cfg->socket_path) - 1] = '\0';
            } else if (strcmp(key, "listen_udp") == 0) {
                cfg->listen_udp = (strcmp(val, "true") == 0 || strcmp(val, "1") == 0) ? 1 : 0;
            } else if (strcmp(key, "udp_port") == 0) {
                char *endp;
                long port = strtol(val, &endp, 10);
                if (endp != val && *endp == '\0' && port > 0 && port <= 65535)
                    cfg->udp_port = (int)port;
            }
        } else if (indent >= 2) {
            /* Nested key under a section */
            if (strcmp(section, "forward") == 0) {
                if (strcmp(key, "enabled") == 0) {
                    cfg->forward_enabled = (strcmp(val, "true") == 0 || strcmp(val, "1") == 0) ? 1 : 0;
                } else if (strcmp(key, "host") == 0) {
                    strncpy(cfg->forward_host, val, sizeof(cfg->forward_host) - 1);
                    cfg->forward_host[sizeof(cfg->forward_host) - 1] = '\0';
                } else if (strcmp(key, "port") == 0) {
                    char *endp;
                    long port = strtol(val, &endp, 10);
                    if (endp != val && *endp == '\0' && port > 0 && port <= 65535)
                        cfg->forward_port = (int)port;
                }
            }
        }
    }

    fclose(f);
    return 0;
}

/* -------------------------------------------------------------------------
 * Facility / severity helpers
 * ---------------------------------------------------------------------- */

static const char *facility_name(int fac)
{
    switch (fac) {
    case FAC_KERN:   return "kern";
    case FAC_USER:   return "user";
    case FAC_MAIL:   return "mail";
    case FAC_DAEMON: return "daemon";
    case FAC_AUTH:   return "auth";
    case FAC_SYSLOG: return "syslog";
    case FAC_LPR:    return "lpr";
    case FAC_NEWS:   return "news";
    case FAC_UUCP:   return "uucp";
    case FAC_CRON:   return "cron";
    case FAC_LOCAL0: return "local0";
    case FAC_LOCAL1: return "local1";
    case FAC_LOCAL2: return "local2";
    case FAC_LOCAL3: return "local3";
    case FAC_LOCAL4: return "local4";
    case FAC_LOCAL5: return "local5";
    case FAC_LOCAL6: return "local6";
    case FAC_LOCAL7: return "local7";
    default:         return "user";
    }
}

static const char *severity_name(int sev)
{
    switch (sev) {
    case SEV_EMERG:   return "emerg";
    case SEV_ALERT:   return "alert";
    case SEV_CRIT:    return "crit";
    case SEV_ERR:     return "err";
    case SEV_WARNING: return "warning";
    case SEV_NOTICE:  return "notice";
    case SEV_INFO:    return "info";
    case SEV_DEBUG:   return "debug";
    default:          return "info";
    }
}

/* -------------------------------------------------------------------------
 * RFC 3164 message parsing
 * Format: <PRI>TIMESTAMP HOSTNAME TAG[PID]: MSG
 *     or: <PRI>MSG  (abbreviated form from local apps)
 * ---------------------------------------------------------------------- */

struct syslog_msg {
    int  priority;           /* raw PRI value */
    int  facility;           /* decoded facility */
    int  severity;           /* decoded severity */
    char timestamp[32];      /* parsed or generated timestamp */
    char hostname[MAX_HOST]; /* source hostname */
    char tag[MAX_TAG];       /* program name / tag */
    int  pid;                /* program PID, or -1 */
    char *message;           /* pointer into buf at message start */
    char buf[MAX_MSG + 64];  /* storage for modified message data */
};

/*
 * Attempt to parse an RFC 3164 syslog message.
 * If parsing fails at any stage, sensible defaults are used.
 * Returns 0 on success, -1 if the message should be discarded.
 */
static int parse_syslog(const char *raw, size_t raw_len, struct syslog_msg *msg)
{
    const char *p = raw;
    const char *end;
    size_t copy_len;

    if (raw_len == 0)
        return -1;

    memset(msg, 0, sizeof(*msg));
    msg->pid      = -1;
    msg->facility = FAC_USER;
    msg->severity = SEV_INFO;

    /* Copy raw message into the message buffer (NUL-terminated) */
    copy_len = (raw_len < MAX_MSG) ? raw_len : MAX_MSG - 1;
    memcpy(msg->buf, raw, copy_len);
    msg->buf[copy_len] = '\0';
    p = msg->buf;

    /* Strip trailing newlines */
    end = p + strlen(p);
    while (end > p && (*(end - 1) == '\n' || *(end - 1) == '\r'))
        end--;
    copy_len = (size_t)(end - p);
    if (p != msg->buf) {
        memmove(msg->buf, p, copy_len);
        msg->buf[copy_len] = '\0';
    }
    p = msg->buf;

    /* Parse <PRI> if present */
    if (*p == '<') {
        const char *gt = strchr(p, '>');
        if (gt && gt - p <= 5) {
            char pri_str[8];
            size_t pri_len = (size_t)(gt - p - 1);
            if (pri_len < sizeof(pri_str)) {
                memcpy(pri_str, p + 1, pri_len);
                pri_str[pri_len] = '\0';
                msg->priority = atoi(pri_str);
                msg->facility = msg->priority >> 3;
                msg->severity = msg->priority & 0x07;
                /* Clamp facility to known range */
                if (msg->facility > 23)
                    msg->facility = FAC_USER;
            }
            p = gt + 1;
        }
    }

    /* Generate current timestamp for output (we always use local time) */
    {
        time_t now = time(NULL);
        struct tm *tm_info = localtime(&now);
        strftime(msg->timestamp, sizeof(msg->timestamp), "%b %e %H:%M:%S", tm_info);
    }

    /*
     * Try to skip an RFC 3164 timestamp: "Mmm DD HH:MM:SS "
     * Month abbreviation (3 chars) + space + day (1-2 digits) + space + time
     * Minimum: "Jan  1 00:00:00 " = 16 chars
     */
    if (strlen(p) >= 16) {
        /* Quick check: 3 alpha chars, space, then digit or space */
        if (p[3] == ' ' && (p[4] == ' ' || (p[4] >= '0' && p[4] <= '9'))) {
            /* Looks like a timestamp — skip 15 chars + trailing space */
            const char *ts_end = p + 15;
            if (*ts_end == ' ') {
                /* Copy timestamp into msg->timestamp for fidelity */
                memcpy(msg->timestamp, p, 15);
                msg->timestamp[15] = '\0';
                p = ts_end + 1;
            }
        }
    }

    /* Try to parse hostname: next word up to space */
    {
        const char *sp = strchr(p, ' ');
        if (sp) {
            size_t hlen = (size_t)(sp - p);
            if (hlen > 0 && hlen < MAX_HOST) {
                memcpy(msg->hostname, p, hlen);
                msg->hostname[hlen] = '\0';
                p = sp + 1;
            }
        }
    }

    /* If hostname looks like a tag (contains ':' or '[') it wasn't a hostname */
    if (strchr(msg->hostname, ':') || strchr(msg->hostname, '[')) {
        /* Rewind — there was no hostname in this message, use our own */
        p = msg->buf;
        if (*p == '<') {
            const char *gt = strchr(p, '>');
            if (gt) p = gt + 1;
        }
        /* Skip potential timestamp again */
        if (strlen(p) >= 16 && p[3] == ' ' && (p[4] == ' ' || (p[4] >= '0' && p[4] <= '9'))) {
            const char *ts_end = p + 15;
            if (*ts_end == ' ') p = ts_end + 1;
        }
        strncpy(msg->hostname, g_hostname, sizeof(msg->hostname) - 1);
        msg->hostname[sizeof(msg->hostname) - 1] = '\0';
    } else if (msg->hostname[0] == '\0') {
        strncpy(msg->hostname, g_hostname, sizeof(msg->hostname) - 1);
        msg->hostname[sizeof(msg->hostname) - 1] = '\0';
    }

    /* Try to parse tag[pid]: or tag: */
    {
        const char *colon = strchr(p, ':');
        const char *bracket_open  = strchr(p, '[');
        const char *bracket_close = strchr(p, ']');

        if (colon && (!bracket_open || bracket_open < colon)) {
            /* Has bracket: tag[pid]: msg */
            if (bracket_open && bracket_close && bracket_close < colon) {
                size_t tag_len = (size_t)(bracket_open - p);
                if (tag_len < MAX_TAG) {
                    memcpy(msg->tag, p, tag_len);
                    msg->tag[tag_len] = '\0';
                }
                /* Parse PID between brackets */
                {
                    char pid_str[16];
                    size_t pid_len = (size_t)(bracket_close - bracket_open - 1);
                    if (pid_len < sizeof(pid_str)) {
                        char *endp;
                        long parsed_pid;
                        memcpy(pid_str, bracket_open + 1, pid_len);
                        pid_str[pid_len] = '\0';
                        parsed_pid = strtol(pid_str, &endp, 10);
                        if (endp != pid_str && *endp == '\0' && parsed_pid > 0)
                            msg->pid = (int)parsed_pid;
                    }
                }
                p = colon + 1;
                if (*p == ' ') p++;
            } else {
                /* No bracket: tag: msg */
                size_t tag_len = (size_t)(colon - p);
                if (tag_len < MAX_TAG) {
                    memcpy(msg->tag, p, tag_len);
                    msg->tag[tag_len] = '\0';
                }
                p = colon + 1;
                if (*p == ' ') p++;
            }
        } else if (msg->tag[0] == '\0') {
            strncpy(msg->tag, "unknown", sizeof(msg->tag) - 1);
            msg->tag[sizeof(msg->tag) - 1] = '\0';
        }
    }

    /* Remainder is the message body */
    msg->message = (char *)p;

    return 0;
}

/* -------------------------------------------------------------------------
 * Log file management
 * ---------------------------------------------------------------------- */

static int open_log_file(const char *path)
{
    int fd;
    struct stat st;

    YAP_DBG("open_log_file: path=%s", path);

    /* Ensure parent directory exists */
    char dir[MAX_PATH];
    strncpy(dir, path, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = '\0';
    char *slash = strrchr(dir, '/');
    if (slash && slash != dir) {
        *slash = '\0';
        YAP_DBG("open_log_file: checking parent dir=%s", dir);
        if (stat(dir, &st) != 0) {
            YAP_DBG("open_log_file: parent missing, mkdir(%s)", dir);
            if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
                fprintf(stderr, "yap: cannot create log directory %s: %s\n", dir, strerror(errno));
                YAP_DBG("open_log_file: mkdir FAILED errno=%d (%s)", errno, strerror(errno));
                return -1;
            }
            YAP_DBG("open_log_file: mkdir OK");
        } else if (!S_ISDIR(st.st_mode)) {
            fprintf(stderr, "yap: log parent %s exists but is not a directory\n", dir);
            YAP_DBG("open_log_file: parent not a directory");
            return -1;
        } else {
            YAP_DBG("open_log_file: parent dir exists");
        }
    }

    YAP_DBG("open_log_file: calling open(%s)", path);
    fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0640);
    if (fd < 0) {
        fprintf(stderr, "yap: cannot open log file %s: %s\n", path, strerror(errno));
        YAP_DBG("open_log_file: open FAILED fd=%d errno=%d (%s)", fd, errno, strerror(errno));
    } else {
        YAP_DBG("open_log_file: open OK fd=%d", fd);
    }
    return fd;
}

#if YAP_BLUEYOS_COMPAT
static int mkdir_p(const char *path, mode_t mode)
{
    char tmp[MAX_PATH];
    char *p;
    struct stat st;

    YAP_DBG("mkdir_p: path=%s", path);

    if (!path || path[0] == '\0') {
        errno = EINVAL;
        return -1;
    }
    if (strlen(path) >= sizeof(tmp)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    memcpy(tmp, path, strlen(path) + 1);

    for (p = tmp + 1; *p; p++) {
        if (*p != '/')
            continue;

        *p = '\0';
        YAP_DBG("mkdir_p: component=%s", tmp);
        if (mkdir(tmp, mode) != 0 && errno != EEXIST) {
            YAP_DBG("mkdir_p: mkdir(%s) FAILED errno=%d (%s)", tmp, errno, strerror(errno));
            return -1;
        }
        if (stat(tmp, &st) != 0 || !S_ISDIR(st.st_mode)) {
            YAP_DBG("mkdir_p: stat(%s) not a dir errno=%d", tmp, errno);
            errno = ENOTDIR;
            return -1;
        }
        *p = '/';
    }

    YAP_DBG("mkdir_p: final mkdir(%s)", tmp);
    if (mkdir(tmp, mode) != 0 && errno != EEXIST) {
        YAP_DBG("mkdir_p: final mkdir FAILED errno=%d (%s)", errno, strerror(errno));
        return -1;
    }
    if (stat(tmp, &st) != 0 || !S_ISDIR(st.st_mode)) {
        YAP_DBG("mkdir_p: final stat not a dir errno=%d", errno);
        errno = ENOTDIR;
        return -1;
    }

    YAP_DBG("mkdir_p: OK");
    return 0;
}

static int ensure_parent_dir(const char *path, mode_t mode)
{
    char parent[MAX_PATH];
    char *slash;

    if (!path || path[0] == '\0')
        return 0;
    if (strlen(path) >= sizeof(parent)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    memcpy(parent, path, strlen(path) + 1);
    slash = strrchr(parent, '/');
    if (!slash || slash == parent)
        return 0;
    *slash = '\0';

    return mkdir_p(parent, mode);
}
#endif

static void close_log_file(void)
{
    if (g_log_fd >= 0) {
        close(g_log_fd);
        g_log_fd = -1;
    }
}

static void reopen_log_file(void)
{
    close_log_file();
    g_log_fd = open_log_file(g_cfg.log_file);
    if (g_log_fd >= 0)
        yap_log("log file reopened — Bluey's ready to yap again!");
}

/* -------------------------------------------------------------------------
 * Write a formatted syslog message to the log file
 * Format: TIMESTAMP HOSTNAME TAG[PID]: [facility.severity] MESSAGE
 * ---------------------------------------------------------------------- */

static void write_log_entry(const struct syslog_msg *msg)
{
    int len;

    if (g_log_fd < 0)
        return;

    if (msg->pid >= 0) {
        len = snprintf(g_write_buf, sizeof(g_write_buf),
                       "%s %s %s[%d]: <%s.%s> %s\n",
                       msg->timestamp,
                       msg->hostname,
                       msg->tag,
                       msg->pid,
                       facility_name(msg->facility),
                       severity_name(msg->severity),
                       msg->message);
    } else {
        len = snprintf(g_write_buf, sizeof(g_write_buf),
                       "%s %s %s: <%s.%s> %s\n",
                       msg->timestamp,
                       msg->hostname,
                       msg->tag,
                       facility_name(msg->facility),
                       severity_name(msg->severity),
                       msg->message);
    }

    if (len < 0)
        return;
    if ((size_t)len >= sizeof(g_write_buf))
        len = (int)sizeof(g_write_buf) - 1;

    write_log_bytes(g_write_buf, (size_t)len);
}

/* -------------------------------------------------------------------------
 * Remote syslog forwarding
 * Forwards messages as RFC 3164 UDP syslog datagrams.
 * ---------------------------------------------------------------------- */

static int setup_forward_socket(void)
{
    if (!g_cfg.forward_enabled || g_cfg.forward_host[0] == '\0')
        return -1;

#if !YAP_HAVE_INET_SOCKETS
    yap_log("remote forwarding disabled in BlueyOS compatibility mode");
    return -1;
#else

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        yap_log("cannot create forwarding socket: %s", strerror(errno));
        return -1;
    }

    /* Set non-blocking so a stuck remote doesn't stall us */
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0)
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    return fd;
#endif
}

static void forward_message(const struct syslog_msg *msg, const char *raw, size_t raw_len)
{
#if !YAP_HAVE_INET_SOCKETS
    (void)msg;
    (void)raw;
    (void)raw_len;
    return;
#else
    char fwd_buf[MAX_MSG + 32];
    int  fwd_len;
    struct sockaddr_in dest;
    struct hostent *he;

    if (g_fwd_fd < 0)
        return;

    /* Reconstruct RFC 3164 message: <PRI>TIMESTAMP HOSTNAME TAG: MSG */
    if (msg->pid >= 0) {
        fwd_len = snprintf(fwd_buf, sizeof(fwd_buf), "<%d>%s %s %s[%d]: %s",
                           msg->priority,
                           msg->timestamp, msg->hostname,
                           msg->tag, msg->pid,
                           msg->message);
    } else {
        fwd_len = snprintf(fwd_buf, sizeof(fwd_buf), "<%d>%s %s %s: %s",
                           msg->priority,
                           msg->timestamp, msg->hostname,
                           msg->tag,
                           msg->message);
    }

    if (fwd_len <= 0)
        return;
    if ((size_t)fwd_len >= sizeof(fwd_buf))
        fwd_len = (int)sizeof(fwd_buf) - 1;

    (void)raw;
    (void)raw_len;

    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_port   = htons((uint16_t)g_cfg.forward_port);

    /* Try inet_pton first, fall back to gethostbyname */
    if (inet_pton(AF_INET, g_cfg.forward_host, &dest.sin_addr) != 1) {
        he = gethostbyname(g_cfg.forward_host);
        if (!he || !he->h_addr_list || !he->h_addr_list[0])
            return;
        memcpy(&dest.sin_addr, he->h_addr_list[0], (size_t)he->h_length);
    }

    sendto(g_fwd_fd, fwd_buf, (size_t)fwd_len, 0,
           (struct sockaddr *)&dest, sizeof(dest));
#endif
}

/* -------------------------------------------------------------------------
 * Local input setup
 * ---------------------------------------------------------------------- */

#if YAP_BLUEYOS_COMPAT
static int remove_spool_path(const char *path)
{
    DIR *dir;
    struct dirent *entry;
    char item_path[MAX_PATH];

    dir = opendir(path);
    if (!dir) {
        /* ENOTDIR or EPERM: path is likely a regular file, not a directory. */
        if (errno == ENOTDIR || errno == EPERM) {
            if (unlink(path) != 0 && errno != ENOENT)
                return -1;
            return 0;
        }
        if (errno == ENOENT)
            return 0;
        return -1;
    }

    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;

        if (snprintf(item_path, sizeof(item_path), "%s/%s", path, entry->d_name) >= (int)sizeof(item_path)) {
            closedir(dir);
            errno = ENAMETOOLONG;
            return -1;
        }

        if (remove_spool_path(item_path) != 0) {
            closedir(dir);
            return -1;
        }
    }

    closedir(dir);

    if (rmdir(path) != 0 && errno != ENOENT)
        return -1;

    return 0;
}

static int clear_spool_dir(const char *path)
{
    DIR *dir;
    struct dirent *entry;
    char item_path[MAX_PATH];

    YAP_DBG("clear_spool_dir: opening %s", path);
    dir = opendir(path);
    if (!dir) {
        YAP_DBG("clear_spool_dir: opendir FAILED errno=%d", errno);
        if (errno == ENOENT)
            return 0;
        return -1;
    }

    YAP_DBG("clear_spool_dir: readdir loop");
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;

        YAP_DBG("clear_spool_dir: entry=%s", entry->d_name);
        if (snprintf(item_path, sizeof(item_path), "%s/%s", path, entry->d_name) >= (int)sizeof(item_path)) {
            closedir(dir);
            errno = ENAMETOOLONG;
            return -1;
        }

        if (remove_spool_path(item_path) != 0) {
            YAP_DBG("clear_spool_dir: remove_spool_path(%s) FAILED errno=%d", item_path, errno);
            closedir(dir);
            return -1;
        }
        YAP_DBG("clear_spool_dir: remove_spool_path OK");
    }

    YAP_DBG("clear_spool_dir: done, closedir");
    closedir(dir);
    return 0;
}

static int setup_local_input(const char *path)
{
    struct stat st;

    YAP_DBG("setup_local_input: path=%s", path);

    if (ensure_parent_dir(path, 0755) != 0) {
        fprintf(stderr, "yap: cannot create spool parent for %s: %s\n", path, strerror(errno));
        YAP_DBG("setup_local_input: ensure_parent_dir FAILED errno=%d", errno);
        return -1;
    }
    YAP_DBG("setup_local_input: ensure_parent_dir OK");

    if (stat(path, &st) == 0) {
        YAP_DBG("setup_local_input: path exists, is_dir=%d", S_ISDIR(st.st_mode));
        if (!S_ISDIR(st.st_mode)) {
            if (unlink(path) != 0) {
                fprintf(stderr, "yap: unlink(%s): %s\n", path, strerror(errno));
                YAP_DBG("setup_local_input: unlink FAILED errno=%d", errno);
                return -1;
            }
            YAP_DBG("setup_local_input: unlink OK");
        } else if (clear_spool_dir(path) != 0) {
            fprintf(stderr, "yap: clear spool %s: %s\n", path, strerror(errno));
            YAP_DBG("setup_local_input: clear_spool_dir FAILED errno=%d", errno);
            return -1;
        } else {
            YAP_DBG("setup_local_input: clear_spool_dir OK");
        }
    } else if (errno != ENOENT) {
        fprintf(stderr, "yap: stat(%s): %s\n", path, strerror(errno));
        YAP_DBG("setup_local_input: stat FAILED unexpected errno=%d", errno);
        return -1;
    } else {
        YAP_DBG("setup_local_input: path does not exist yet (ENOENT), will mkdir");
    }

    YAP_DBG("setup_local_input: calling mkdir(%s)", path);
    if (mkdir(path, YAP_COMPAT_SPOOL_MODE) != 0 && errno != EEXIST) {
        fprintf(stderr, "yap: mkdir(%s): %s\n", path, strerror(errno));
        YAP_DBG("setup_local_input: mkdir FAILED errno=%d (%s)", errno, strerror(errno));
        return -1;
    }
    YAP_DBG("setup_local_input: mkdir OK");

    if (chmod(path, YAP_COMPAT_SPOOL_MODE) != 0) {
        fprintf(stderr, "yap: chmod(%s): %s\n", path, strerror(errno));
        YAP_DBG("setup_local_input: chmod FAILED errno=%d (%s)", errno, strerror(errno));
        return -1;
    }
    YAP_DBG("setup_local_input: chmod OK");

    return YAP_COMPAT_SUCCESS;
}
#else
static int setup_local_input(const char *path)
{
    struct sockaddr_un addr;
    int fd;

    unlink(path);
    fd = socket(AF_UNIX, YAP_LOCAL_SOCKET_TYPE, 0);
    if (fd < 0) {
        fprintf(stderr, "yap: socket(%s): %s\n",
                YAP_LOCAL_SOCKET_LABEL, strerror(errno));
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof(addr.sun_path)) {
        fprintf(stderr, "yap: socket path too long (max %zu): %s\n",
                sizeof(addr.sun_path) - 1, path);
        close(fd);
        return -1;
    }
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "yap: bind(%s): %s\n", path, strerror(errno));
        close(fd);
        return -1;
    }

    chmod(path, 0666);

    return fd;
}
#endif

static int setup_udp_socket(int port)
{
#if !YAP_HAVE_INET_SOCKETS
    fprintf(stderr,
            "yap: UDP syslog disabled in BlueyOS compatibility mode (port %d)\n",
            port);
    return -1;
#else
    struct sockaddr_in addr;
    int fd;
    int opt = 1;

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        fprintf(stderr, "yap: socket(AF_INET): %s\n", strerror(errno));
        return -1;
    }

    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons((uint16_t)port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "yap: bind(UDP %d): %s\n", port, strerror(errno));
        close(fd);
        return -1;
    }

    return fd;
#endif
}

/* -------------------------------------------------------------------------
 * PID file
 * ---------------------------------------------------------------------- */

static void write_pidfile(void)
{
    FILE *f = fopen(YAP_PIDFILE, "w");
    if (!f) {
        fprintf(stderr, "yap: cannot write pidfile %s: %s\n",
                YAP_PIDFILE, strerror(errno));
        return;
    }
    fprintf(f, "%d\n", (int)getpid());
    fclose(f);
}

static void remove_pidfile(void)
{
    unlink(YAP_PIDFILE);
}

static void write_readyfile(void)
{
    int fd = open(YAP_READYFILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    g_ready_written = 1;
    if (fd >= 0) {
        close(fd);
    } else {
        yap_log("cannot create readyfile %s: %s", YAP_READYFILE, strerror(errno));
    }
}

static void remove_readyfile(void)
{
    g_ready_written = 0;
    unlink(YAP_READYFILE);
}

/* -------------------------------------------------------------------------
 * Reload configuration
 * ---------------------------------------------------------------------- */

static void do_reload(void)
{
    struct yap_config new_cfg;
    int old_forward_enabled;
    char old_input_path[MAX_PATH];

    yap_log("reloading configuration — Bluey has more to say!");
    strncpy(old_input_path, g_cfg.socket_path, sizeof(old_input_path) - 1);
    old_input_path[sizeof(old_input_path) - 1] = '\0';

    if (config_load(YAP_CONFIG, &new_cfg) < 0) {
        yap_log("config reload failed — keeping current settings");
        return;
    }

    old_forward_enabled = g_cfg.forward_enabled;

    /* Apply new config */
    g_cfg = new_cfg;

    /* Reopen log file if path changed */
    close_log_file();
    g_log_fd = open_log_file(g_cfg.log_file);

    /* Rebuild forwarding socket if forwarding config changed */
    if (g_fwd_fd >= 0 && !g_cfg.forward_enabled) {
        close(g_fwd_fd);
        g_fwd_fd = -1;
    } else if (g_fwd_fd < 0 && g_cfg.forward_enabled && !old_forward_enabled) {
        g_fwd_fd = setup_forward_socket();
    }

    if (strcmp(old_input_path, g_cfg.socket_path) != 0) {
        int new_unix_fd = -1;

        new_unix_fd = setup_local_input(g_cfg.socket_path);
        if (new_unix_fd < 0) {
            yap_log("cannot reopen local input %s", g_cfg.socket_path);
#if YAP_BLUEYOS_COMPAT
            clear_local_sources();
#endif
            return;
        }

#if !YAP_BLUEYOS_COMPAT
        if (g_unix_fd >= 0) {
            close(g_unix_fd);
            g_unix_fd = -1;
        }
#endif
        g_unix_fd = new_unix_fd;
#if YAP_BLUEYOS_COMPAT
        clear_local_sources();
#endif
    }

    yap_log("configuration reloaded — ready to yap again!");
}

#if YAP_BLUEYOS_COMPAT
static void clear_local_sources(void)
{
    while (g_local_sources) {
        struct local_source *next = g_local_sources->next;
        free(g_local_sources);
        g_local_sources = next;
    }
    g_local_source_count = 0;
    g_local_source_limit_warned = 0;
}

static void process_local_bytes(struct local_source *source,
                                const char *buf,
                                size_t len)
{
    size_t i;

    if (!source)
        return;

    for (i = 0; i < len; i++) {
        char ch = buf[i];

        if (ch == '\n') {
            if (source->line_len > 0)
                process_message(source->line_buf, source->line_len);
            source->line_len = 0;
            continue;
        }

        if (source->line_len + 1 < sizeof(source->line_buf))
            source->line_buf[source->line_len++] = ch;
    }
}

static void poll_local_source(struct local_source *source)
{
    char path[MAX_PATH];
    struct stat st;
    int fd;
    ssize_t n;
    off_t remaining;

    if (!source)
        return;

    if (snprintf(path, sizeof(path), "%s/%s", g_cfg.socket_path, source->name) >= (int)sizeof(path)) {
        yap_log("local source path too long for %s/%s", g_cfg.socket_path, source->name);
        return;
    }

    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        if (errno != ENOENT)
            yap_log("open(%s) failed: %s", path, strerror(errno));
        return;
    }

    if (fstat(fd, &st) != 0) {
        yap_log("fstat(%s) failed: %s", path, strerror(errno));
        close(fd);
        return;
    }

    if (!S_ISREG(st.st_mode)) {
        close(fd);
        return;
    }

    if (st.st_size < source->offset) {
        source->offset = 0;
        source->line_len = 0;
    }

    if (st.st_size <= source->offset) {
        close(fd);
        return;
    }

    if (lseek(fd, source->offset, SEEK_SET) < 0) {
        yap_log("seek(%s) failed: %s", path, strerror(errno));
        close(fd);
        return;
    }

    remaining = st.st_size - source->offset;
    while (remaining > 0) {
        size_t chunk = (remaining > (off_t)sizeof(g_recv_buf)) ? sizeof(g_recv_buf) : (size_t)remaining;
        n = read(fd, g_recv_buf, chunk);
        if (n <= 0)
            break;
        source->offset += n;
        remaining -= n;
        process_local_bytes(source, g_recv_buf, (size_t)n);
    }

    if (n < 0 && errno != EINTR)
        yap_log("read(%s) failed: %s", path, strerror(errno));

    close(fd);
}

static void receive_local_message(void)
{
    DIR *dir;
    struct dirent *entry;
    char path[MAX_PATH];

    dir = opendir(g_cfg.socket_path);
    if (!dir) {
        if (errno != ENOENT)
            yap_log("open spool dir %s failed: %s", g_cfg.socket_path, strerror(errno));
        return;
    }

    while ((entry = readdir(dir)) != NULL) {
        struct local_source *source;

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;

        if (snprintf(path, sizeof(path), "%s/%s", g_cfg.socket_path, entry->d_name) >= (int)sizeof(path))
            continue;

        source = g_local_sources;
        while (source && strcmp(source->name, entry->d_name) != 0)
            source = source->next;

        if (!source) {
            if (g_local_source_count >= YAP_MAX_LOCAL_SOURCES) {
                if (!g_local_source_limit_warned) {
                    yap_log("spool source limit reached (%d); ignoring additional files",
                            YAP_MAX_LOCAL_SOURCES);
                    g_local_source_limit_warned = 1;
                }
                continue;
            }
            source = calloc(1, sizeof(*source));
            if (!source)
                continue;
            if (strlen(entry->d_name) >= sizeof(source->name)) {
                free(source);
                continue;
            }
            bstrncpy(source->name, entry->d_name, sizeof(source->name));
            source->next = g_local_sources;
            g_local_sources = source;
            g_local_source_count++;
        }

        poll_local_source(source);
    }

    closedir(dir);
}
#endif

/* -------------------------------------------------------------------------
 * Process one syslog message
 * ---------------------------------------------------------------------- */

static void process_message(const char *buf, size_t len)
{
    struct syslog_msg msg;

    if (len == 0)
        return;

    if (parse_syslog(buf, len, &msg) < 0)
        return;

    write_log_entry(&msg);

    if (g_cfg.forward_enabled)
        forward_message(&msg, buf, len);
}

/* -------------------------------------------------------------------------
 * Main receive loop
 * ---------------------------------------------------------------------- */

static void receive_loop(void)
{
#if YAP_BLUEYOS_COMPAT
#else
    fd_set rfds;
    int max_fd;
    char buf[MAX_MSG];
    ssize_t n;
#endif

    yap_log("The Magic Claw has chosen yap to receive all the gossip!");
    yap_log("yap v%s listening on %s via %s",
            YAP_VERSION, g_cfg.socket_path, YAP_LOCAL_SOCKET_LABEL);
    if (g_cfg.listen_udp && g_udp_fd >= 0)
        yap_log("also listening on UDP port %d", g_cfg.udp_port);
    else if (g_cfg.listen_udp)
        yap_log("UDP syslog requested but unavailable in this build");
    if (g_cfg.forward_enabled && g_fwd_fd >= 0)
        yap_log("forwarding to %s:%d", g_cfg.forward_host, g_cfg.forward_port);
    else if (g_cfg.forward_enabled)
        yap_log("remote forwarding requested but unavailable in this build");

    write_readyfile();

#if YAP_BLUEYOS_COMPAT
    while (!g_flag_shutdown) {
        if (g_flag_reload) {
            g_flag_reload = 0;
            do_reload();
        }
        if (g_flag_reopen) {
            g_flag_reopen = 0;
            reopen_log_file();
        }

        if (g_unix_fd >= 0)
            receive_local_message();

        sleep(1);
    }
#else
    while (!g_flag_shutdown) {
        /* Handle pending signals */
        if (g_flag_reload) {
            g_flag_reload = 0;
            do_reload();
        }
        if (g_flag_reopen) {
            g_flag_reopen = 0;
            reopen_log_file();
        }

        /* Build fd_set */
        FD_ZERO(&rfds);
        max_fd = -1;

        if (g_unix_fd >= 0) {
            FD_SET(g_unix_fd, &rfds);
            if (g_unix_fd > max_fd) max_fd = g_unix_fd;
        }
        if (g_udp_fd >= 0) {
            FD_SET(g_udp_fd, &rfds);
            if (g_udp_fd > max_fd) max_fd = g_udp_fd;
        }

        if (max_fd < 0) {
            /* No sockets — shouldn't happen, but sleep and retry */
            sleep(1);
            continue;
        }

        /* Wait up to 1 second so we can check signal flags */
        struct timeval tv;
        tv.tv_sec  = 1;
        tv.tv_usec = 0;

        int ready = select(max_fd + 1, &rfds, NULL, NULL, &tv);
        if (ready < 0) {
            if (errno == EINTR)
                continue;
            yap_log("select error: %s", strerror(errno));
            break;
        }
        if (ready == 0)
            continue; /* timeout — loop back to check signals */

        /* Read from UNIX socket */
        if (g_unix_fd >= 0 && FD_ISSET(g_unix_fd, &rfds)) {
            n = recv(g_unix_fd, buf, sizeof(buf) - 1, 0);
            if (n > 0) {
                buf[n] = '\0';
                process_message(buf, (size_t)n);
            }
        }

        /* Read from UDP socket */
#if YAP_HAVE_INET_SOCKETS
        if (g_udp_fd >= 0 && FD_ISSET(g_udp_fd, &rfds)) {
            struct sockaddr_in src;
            socklen_t src_len = sizeof(src);
            n = recvfrom(g_udp_fd, buf, sizeof(buf) - 1, 0,
                         (struct sockaddr *)&src, &src_len);
            if (n > 0) {
                buf[n] = '\0';
                process_message(buf, (size_t)n);
            }
        }
#endif
    }
#endif
}

/* -------------------------------------------------------------------------
 * Daemonize
 * ---------------------------------------------------------------------- */

static void daemonize(void)
{
    pid_t pid;

    pid = fork();
    if (pid < 0) {
        perror("yap: fork");
        exit(EXIT_FAILURE);
    }
    if (pid > 0) {
        /* Parent exits */
        exit(EXIT_SUCCESS);
    }

    /* Child: become session leader */
    if (setsid() < 0) {
        perror("yap: setsid");
        exit(EXIT_FAILURE);
    }

    /* Second fork to prevent reacquiring a controlling terminal */
    pid = fork();
    if (pid < 0) {
        perror("yap: fork2");
        exit(EXIT_FAILURE);
    }
    if (pid > 0) {
        exit(EXIT_SUCCESS);
    }

    /* Redirect stdio to /dev/null */
    int devnull = open("/dev/null", O_RDWR);
    if (devnull >= 0) {
        dup2(devnull, STDIN_FILENO);
        dup2(devnull, STDOUT_FILENO);
        dup2(devnull, STDERR_FILENO);
        if (devnull > STDERR_FILENO)
            close(devnull);
    }

    /* Set safe umask */
    umask(0022);

    /* Change to root directory */
    if (chdir("/") != 0) {
        /* Non-fatal — continue even if chdir fails */
        (void)0;
    }
}

/* -------------------------------------------------------------------------
 * Signal setup
 * ---------------------------------------------------------------------- */

static void setup_signals(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);

    sa.sa_handler = handle_sighup;
    sigaction(SIGHUP, &sa, NULL);

    sa.sa_handler = handle_sigusr1;
    sigaction(SIGUSR1, &sa, NULL);

    sa.sa_handler = handle_sigterm;
    sigaction(SIGTERM, &sa, NULL);

    /* Ignore SIGPIPE (can occur when forwarding to a closed remote) */
    sa.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &sa, NULL);

}

/* -------------------------------------------------------------------------
 * Usage / main
 * ---------------------------------------------------------------------- */

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s [-n] [-c config]\n"
            "  -n        do not daemonize (run in foreground)\n"
            "  -c FILE   configuration file (default: %s)\n"
            "  -h        show this help\n",
            prog, YAP_CONFIG);
}

int main(int argc, char *argv[])
{
    int no_daemon  = 0;
    const char *config_path = YAP_CONFIG;
    int opt;

    YAP_DBG("main: yap starting, argc=%d", argc);

    /* Parse arguments */
    while ((opt = getopt(argc, argv, "nc:h")) != -1) {
        switch (opt) {
        case 'n':
            no_daemon = 1;
            break;
        case 'c':
            config_path = optarg;
            break;
        case 'h':
            usage(argv[0]);
            return EXIT_SUCCESS;
        default:
            usage(argv[0]);
            return EXIT_FAILURE;
        }
    }

    /* Get hostname early so it's available everywhere */
    if (gethostname(g_hostname, sizeof(g_hostname)) != 0) {
        strncpy(g_hostname, "blueyos", sizeof(g_hostname) - 1);
        g_hostname[sizeof(g_hostname) - 1] = '\0';
    }
    YAP_DBG("startup: hostname=%s", g_hostname);

    /* Load configuration */
    YAP_DBG("startup: loading config from %s", config_path);
    if (config_load(config_path, &g_cfg) < 0) {
        fprintf(stderr, "yap: failed to load config %s, using defaults\n", config_path);
        YAP_DBG("startup: config load failed, using defaults");
        config_defaults(&g_cfg);
    }
    YAP_DBG("startup: log_file=%s socket_path=%s", g_cfg.log_file, g_cfg.socket_path);

    /* Open log file before daemonizing so errors go to stderr */
    YAP_DBG("startup: opening log file");
    g_log_fd = open_log_file(g_cfg.log_file);
    if (g_log_fd < 0) {
        fprintf(stderr, "yap: cannot open log file %s — aborting\n", g_cfg.log_file);
        YAP_DBG("startup: open_log_file FAILED, aborting");
        return EXIT_FAILURE;
    }
    YAP_DBG("startup: log file open OK fd=%d", g_log_fd);

    /* Set up local input */
    YAP_DBG("startup: setting up local input");
    g_unix_fd = setup_local_input(g_cfg.socket_path);
    if (g_unix_fd < 0) {
        fprintf(stderr, "yap: cannot set up %s — aborting\n", g_cfg.socket_path);
        YAP_DBG("startup: setup_local_input FAILED, aborting");
        return EXIT_FAILURE;
    }
    YAP_DBG("startup: local input OK fd=%d", g_unix_fd);

    /* Set up UDP socket if requested */
    if (g_cfg.listen_udp) {
        g_udp_fd = setup_udp_socket(g_cfg.udp_port);
        /* Non-fatal if UDP fails */
    }

    /* Set up forwarding socket */
    if (g_cfg.forward_enabled)
        g_fwd_fd = setup_forward_socket();

    /* Set up signal handlers */
    setup_signals();

    /* Daemonize (unless -n) */
    if (!no_daemon)
        daemonize();

    /* Write PID file */
    write_pidfile();

    /* Startup banner */
    yap_log("yap v%s starting up — Let me tell you about my day!", YAP_VERSION);

    /* Enter main receive loop */
    receive_loop();

    /* Clean shutdown */
    yap_log("yap shutting down — That was a big day, wasn't it!");
    remove_readyfile();

    close_log_file();

    if (g_unix_fd >= 0) {
#if YAP_BLUEYOS_COMPAT
        clear_local_sources();
        (void)clear_spool_dir(g_cfg.socket_path);
        (void)rmdir(g_cfg.socket_path);
#else
        close(g_unix_fd);
        unlink(g_cfg.socket_path);
#endif
    }
    if (g_udp_fd >= 0)  close(g_udp_fd);
    if (g_fwd_fd >= 0)  close(g_fwd_fd);

    remove_pidfile();

    return EXIT_SUCCESS;
}
