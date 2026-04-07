/* Feature-test macros — must appear before any system headers */
#define _POSIX_C_SOURCE 200809L

/*
 * yap - syslog daemon for BlueyOS
 * "Let me tell you about my day!" - Bluey Heeler
 *
 * yap is a standards-compliant syslog daemon for BlueyOS. It listens on
 * /dev/log for syslog messages from local applications, optionally accepts
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

/* -------------------------------------------------------------------------
 * Constants
 * ---------------------------------------------------------------------- */

#define YAP_VERSION     "0.1.0"
#define YAP_CONFIG      "/etc/yap.yml"
#define YAP_PIDFILE     "/var/run/yap.pid"
#define YAP_SOCKET_PATH "/dev/log"
#define YAP_LOGFILE     "/var/log/system.log"
#define YAP_UDP_PORT    514

#define MAX_MSG     8192   /* maximum incoming syslog message size */
#define MAX_PATH    512    /* maximum path length */
#define MAX_HOST    256    /* maximum hostname length */
#define MAX_LINE    1024   /* maximum line length in config/output */
#define MAX_TAG     128    /* maximum syslog tag length */

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
    char socket_path[MAX_PATH];/* /dev/log UNIX socket path */
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
static int  g_unix_fd  = -1;   /* /dev/log socket fd */
static int  g_udp_fd   = -1;   /* UDP socket fd */
static int  g_fwd_fd   = -1;   /* forwarding socket fd */
static char g_hostname[MAX_HOST];

/* Signal flags — set by signal handlers, checked in main loop */
static volatile sig_atomic_t g_flag_reload   = 0;
static volatile sig_atomic_t g_flag_reopen   = 0;
static volatile sig_atomic_t g_flag_shutdown = 0;

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

    /* Write to log file if open, otherwise stderr */
    if (g_log_fd >= 0) {
        ssize_t written = write(g_log_fd, buf, strlen(buf));
        (void)written;
    } else {
        fputs(buf, stderr);
    }
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
    strncpy(cfg->log_file,     YAP_LOGFILE,     sizeof(cfg->log_file) - 1);
    strncpy(cfg->socket_path,  YAP_SOCKET_PATH, sizeof(cfg->socket_path) - 1);
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
            } else if (strcmp(key, "socket") == 0) {
                strncpy(cfg->socket_path, val, sizeof(cfg->socket_path) - 1);
            } else if (strcmp(key, "listen_udp") == 0) {
                cfg->listen_udp = (strcmp(val, "true") == 0 || strcmp(val, "1") == 0) ? 1 : 0;
            } else if (strcmp(key, "udp_port") == 0) {
                int port = atoi(val);
                if (port > 0 && port <= 65535)
                    cfg->udp_port = port;
            }
        } else if (indent >= 2) {
            /* Nested key under a section */
            if (strcmp(section, "forward") == 0) {
                if (strcmp(key, "enabled") == 0) {
                    cfg->forward_enabled = (strcmp(val, "true") == 0 || strcmp(val, "1") == 0) ? 1 : 0;
                } else if (strcmp(key, "host") == 0) {
                    strncpy(cfg->forward_host, val, sizeof(cfg->forward_host) - 1);
                } else if (strcmp(key, "port") == 0) {
                    int port = atoi(val);
                    if (port > 0 && port <= 65535)
                        cfg->forward_port = port;
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
    char tmp[MAX_MSG];
    size_t copy_len;

    if (raw_len == 0)
        return -1;

    memset(msg, 0, sizeof(*msg));
    msg->pid      = -1;
    msg->facility = FAC_USER;
    msg->severity = SEV_INFO;

    /* Copy raw message into working buffer (NUL-terminated) */
    copy_len = (raw_len < MAX_MSG) ? raw_len : MAX_MSG - 1;
    memcpy(tmp, raw, copy_len);
    tmp[copy_len] = '\0';
    p = tmp;

    /* Strip trailing newlines */
    end = p + strlen(p);
    while (end > p && (*(end - 1) == '\n' || *(end - 1) == '\r'))
        end--;
    copy_len = (size_t)(end - p);
    memcpy(msg->buf, p, copy_len);
    msg->buf[copy_len] = '\0';
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
    } else if (msg->hostname[0] == '\0') {
        strncpy(msg->hostname, g_hostname, sizeof(msg->hostname) - 1);
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
                        memcpy(pid_str, bracket_open + 1, pid_len);
                        pid_str[pid_len] = '\0';
                        msg->pid = atoi(pid_str);
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

    /* Ensure parent directory exists */
    char dir[MAX_PATH];
    strncpy(dir, path, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = '\0';
    char *slash = strrchr(dir, '/');
    if (slash && slash != dir) {
        *slash = '\0';
        /* mkdir -p equivalent for the log dir — ignore errors if it exists */
        mkdir(dir, 0755);
    }

    fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0640);
    if (fd < 0) {
        fprintf(stderr, "yap: cannot open log file %s: %s\n", path, strerror(errno));
    }
    return fd;
}

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
    char line[MAX_MSG + MAX_LINE];
    int n;

    if (g_log_fd < 0)
        return;

    if (msg->pid >= 0) {
        n = snprintf(line, sizeof(line), "%s %s %s[%d]: <%s.%s> %s\n",
                     msg->timestamp,
                     msg->hostname,
                     msg->tag,
                     msg->pid,
                     facility_name(msg->facility),
                     severity_name(msg->severity),
                     msg->message);
    } else {
        n = snprintf(line, sizeof(line), "%s %s %s: <%s.%s> %s\n",
                     msg->timestamp,
                     msg->hostname,
                     msg->tag,
                     facility_name(msg->facility),
                     severity_name(msg->severity),
                     msg->message);
    }

    if (n <= 0)
        return;

    /* Clamp to buffer size */
    if ((size_t)n >= sizeof(line))
        n = (int)sizeof(line) - 1;

    ssize_t written = write(g_log_fd, line, (size_t)n);
    (void)written;
}

/* -------------------------------------------------------------------------
 * Remote syslog forwarding
 * Forwards messages as RFC 3164 UDP syslog datagrams.
 * ---------------------------------------------------------------------- */

static int setup_forward_socket(void)
{
    if (!g_cfg.forward_enabled || g_cfg.forward_host[0] == '\0')
        return -1;

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
}

static void forward_message(const struct syslog_msg *msg, const char *raw, size_t raw_len)
{
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
        if (!he)
            return;
        memcpy(&dest.sin_addr, he->h_addr_list[0], (size_t)he->h_length);
    }

    sendto(g_fwd_fd, fwd_buf, (size_t)fwd_len, 0,
           (struct sockaddr *)&dest, sizeof(dest));
}

/* -------------------------------------------------------------------------
 * Socket setup
 * ---------------------------------------------------------------------- */

static int setup_unix_socket(const char *path)
{
    struct sockaddr_un addr;
    int fd;

    /* Remove stale socket */
    unlink(path);

    fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd < 0) {
        fprintf(stderr, "yap: socket(AF_UNIX): %s\n", strerror(errno));
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    /* sun_path is typically 108 bytes; check the path fits */
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

    /* Allow any process to write to the socket */
    chmod(path, 0666);

    return fd;
}

static int setup_udp_socket(int port)
{
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

/* -------------------------------------------------------------------------
 * Reload configuration
 * ---------------------------------------------------------------------- */

static void do_reload(void)
{
    struct yap_config new_cfg;
    int old_forward_enabled;

    yap_log("reloading configuration — Bluey has more to say!");

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

    yap_log("configuration reloaded — ready to yap again!");
}

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
    char buf[MAX_MSG];
    ssize_t n;
    fd_set rfds;
    int max_fd;

    yap_log("The Magic Claw has chosen yap to receive all the gossip!");
    yap_log("yap v%s listening on %s", YAP_VERSION, g_cfg.socket_path);
    if (g_cfg.listen_udp)
        yap_log("also listening on UDP port %d", g_cfg.udp_port);
    if (g_cfg.forward_enabled)
        yap_log("forwarding to %s:%d", g_cfg.forward_host, g_cfg.forward_port);

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
    }
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

    /* Load configuration */
    if (config_load(config_path, &g_cfg) < 0) {
        fprintf(stderr, "yap: failed to load config %s, using defaults\n", config_path);
        config_defaults(&g_cfg);
    }

    /* Open log file before daemonizing so errors go to stderr */
    g_log_fd = open_log_file(g_cfg.log_file);
    if (g_log_fd < 0) {
        fprintf(stderr, "yap: cannot open log file %s — aborting\n", g_cfg.log_file);
        return EXIT_FAILURE;
    }

    /* Set up UNIX socket */
    g_unix_fd = setup_unix_socket(g_cfg.socket_path);
    if (g_unix_fd < 0) {
        fprintf(stderr, "yap: cannot set up %s — aborting\n", g_cfg.socket_path);
        return EXIT_FAILURE;
    }

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

    close_log_file();

    if (g_unix_fd >= 0) {
        close(g_unix_fd);
        unlink(g_cfg.socket_path);
    }
    if (g_udp_fd >= 0)  close(g_udp_fd);
    if (g_fwd_fd >= 0)  close(g_fwd_fd);

    remove_pidfile();

    return EXIT_SUCCESS;
}
