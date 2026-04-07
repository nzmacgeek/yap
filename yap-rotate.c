/* Feature-test macros — must appear before any system headers */
#define _POSIX_C_SOURCE 200809L

/*
 * yap-rotate - log rotation utility for yap (BlueyOS syslog daemon)
 * "Time to tidy up the backyard!" - Chilli Heeler
 *
 * yap-rotate checks the size and age of yap's log files and rotates them
 * when they exceed configured thresholds. It is intended to be called
 * periodically as a claw timer service.
 *
 * After rotating a log file, it sends SIGUSR1 to yap so it reopens
 * the log file descriptor.
 *
 * Built as i386 ELF, statically linked against musl-blueyos by default.
 *
 * Bluey and all related characters are trademarks of Ludo Studio Pty Ltd,
 * licensed by BBC Studios. This is an unofficial fan/research project with
 * no affiliation to Ludo Studio or the BBC.
 *
 * ⚠️  VIBE CODED RESEARCH PROJECT — NOT FOR PRODUCTION USE ⚠️
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>

/* -------------------------------------------------------------------------
 * Constants
 * ---------------------------------------------------------------------- */

#define YAP_VERSION     "0.1.0"
#define YAP_CONFIG      "/etc/yap.yml"
#define YAP_PIDFILE     "/var/run/yap.pid"
#define YAP_LOGFILE     "/var/log/system.log"

#define MAX_PATH        512
#define MAX_LINE        1024
/* Rotated filename needs room for path + "." + ISO timestamp + "." + seq */
#define MAX_ROTATED     (MAX_PATH + 64)

/* Defaults for rotation */
#define DEFAULT_MAX_SIZE   (10 * 1024 * 1024)  /* 10 MiB */
#define DEFAULT_MAX_AGE    (7 * 24 * 3600)      /* 7 days */
#define DEFAULT_KEEP_COUNT 5

/* -------------------------------------------------------------------------
 * Configuration
 * ---------------------------------------------------------------------- */

struct rotate_config {
    char log_file[MAX_PATH];
    int  enabled;
    long max_size;    /* bytes;  <= 0 means disabled */
    long max_age;     /* seconds; <= 0 means disabled */
    int  keep_count;  /* rotated files to retain */
};

/* -------------------------------------------------------------------------
 * Minimal YAML config loader (same idiom as yap.c)
 * ---------------------------------------------------------------------- */

static char *trim(char *s)
{
    char *end;
    while (*s == ' ' || *s == '\t') s++;
    end = s + strlen(s);
    while (end > s && (*(end-1)==' '||*(end-1)=='\t'||
                       *(end-1)=='\r'||*(end-1)=='\n'))
        end--;
    *end = '\0';
    return s;
}

static void config_defaults(struct rotate_config *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    strncpy(cfg->log_file, YAP_LOGFILE, sizeof(cfg->log_file) - 1);
    cfg->enabled    = 1;
    cfg->max_size   = DEFAULT_MAX_SIZE;
    cfg->max_age    = DEFAULT_MAX_AGE;
    cfg->keep_count = DEFAULT_KEEP_COUNT;
}

static int config_load(const char *path, struct rotate_config *cfg)
{
    FILE  *f;
    char   line[MAX_LINE];
    char   section[64] = "";

    config_defaults(cfg);

    f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "yap-rotate: cannot open config %s: %s\n",
                path, strerror(errno));
        return -1;
    }

    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        int   indent = 0;

        while (*p == ' ') { indent++; p++; }
        if (*p == '#' || *p == '\n' || *p == '\r' || *p == '\0')
            continue;

        char *colon = strchr(p, ':');
        if (!colon) continue;

        *colon = '\0';
        char *key = trim(p);
        char *val = trim(colon + 1);

        /* Section header */
        if (indent == 0 && *val == '\0') {
            strncpy(section, key, sizeof(section) - 1);
            section[sizeof(section) - 1] = '\0';
            continue;
        }

        /* Strip inline comment */
        char *hash = strchr(val, '#');
        if (hash && hash > val && (*(hash-1)==' '||*(hash-1)=='\t')) {
            *hash = '\0';
            val = trim(val);
        }

        if (indent == 0) {
            if (strcmp(key, "log_file") == 0)
                strncpy(cfg->log_file, val, sizeof(cfg->log_file) - 1);
        } else if (indent >= 2 && strcmp(section, "rotation") == 0) {
            if (strcmp(key, "enabled") == 0) {
                cfg->enabled = (strcmp(val,"true")==0||strcmp(val,"1")==0) ? 1 : 0;
            } else if (strcmp(key, "max_size") == 0) {
                cfg->max_size = atol(val);
            } else if (strcmp(key, "max_age") == 0) {
                cfg->max_age = atol(val);
            } else if (strcmp(key, "keep_count") == 0) {
                int k = atoi(val);
                if (k > 0) cfg->keep_count = k;
            }
        }
    }

    fclose(f);
    return 0;
}

/* -------------------------------------------------------------------------
 * PID file reading — used to signal yap
 * ---------------------------------------------------------------------- */

static pid_t read_yap_pid(void)
{
    FILE *f = fopen(YAP_PIDFILE, "r");
    if (!f) return -1;

    pid_t pid = -1;
    if (fscanf(f, "%d", &pid) != 1)
        pid = -1;
    fclose(f);
    return pid;
}

/* -------------------------------------------------------------------------
 * Rotation logic
 * ---------------------------------------------------------------------- */

/*
 * Build a rotated filename: log_file + "." + ISO-8601 timestamp + sequence.
 * e.g. /var/log/system.log.20240101T120000.0
 */
static void build_rotated_name(const char *log_file, char *out, size_t out_size,
                                int seq)
{
    time_t now = time(NULL);
    struct tm *tm_info = gmtime(&now);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y%m%dT%H%M%S", tm_info);
    snprintf(out, out_size, "%s.%s.%d", log_file, ts, seq);
}

/*
 * Return 1 if rotation is needed, 0 otherwise.
 */
static int rotation_needed(const struct rotate_config *cfg)
{
    struct stat st;

    if (!cfg->enabled) return 0;

    if (stat(cfg->log_file, &st) < 0) {
        /* File doesn't exist — nothing to rotate */
        return 0;
    }

    if (cfg->max_size > 0 && st.st_size >= (off_t)cfg->max_size) {
        fprintf(stderr, "yap-rotate: %s exceeds max size (%ld bytes)\n",
                cfg->log_file, cfg->max_size);
        return 1;
    }

    if (cfg->max_age > 0) {
        time_t now = time(NULL);
        if (now - st.st_mtime >= (time_t)cfg->max_age) {
            fprintf(stderr, "yap-rotate: %s exceeds max age (%ld seconds)\n",
                    cfg->log_file, cfg->max_age);
            return 1;
        }
    }

    return 0;
}

/*
 * Rotate the log file:
 *   1. Rename current log to a timestamped archive name.
 *   2. Send SIGUSR1 to yap to reopen the log file.
 *   3. Remove old rotated files beyond keep_count.
 */
static int do_rotate(const struct rotate_config *cfg)
{
    char rotated[MAX_ROTATED];
    int  seq = 0;

    /* Find a unique rotation filename */
    do {
        build_rotated_name(cfg->log_file, rotated, sizeof(rotated), seq);
        seq++;
    } while (access(rotated, F_OK) == 0 && seq < 1000);

    if (seq >= 1000) {
        fprintf(stderr, "yap-rotate: could not find unique rotation name\n");
        return -1;
    }

    /* Rename the live log file */
    if (rename(cfg->log_file, rotated) < 0) {
        fprintf(stderr, "yap-rotate: rename(%s, %s): %s\n",
                cfg->log_file, rotated, strerror(errno));
        return -1;
    }

    printf("yap-rotate: rotated %s -> %s\n", cfg->log_file, rotated);

    /* Signal yap to reopen the log file */
    pid_t pid = read_yap_pid();
    if (pid > 0) {
        if (kill(pid, SIGUSR1) < 0) {
            fprintf(stderr, "yap-rotate: kill(%d, SIGUSR1): %s\n",
                    (int)pid, strerror(errno));
            /* Non-fatal — yap will reopen on next SIGHUP at worst */
        } else {
            printf("yap-rotate: signalled yap (pid %d) to reopen log\n", (int)pid);
        }
    } else {
        fprintf(stderr, "yap-rotate: could not read yap pid from %s\n", YAP_PIDFILE);
    }

    /* Prune old rotated files */
    if (cfg->keep_count > 0) {
        /* Collect all rotated files matching log_file.* */
        char dir[MAX_PATH];
        char base[MAX_PATH];
        char prefix[MAX_PATH];

        strncpy(dir,  cfg->log_file, sizeof(dir) - 1);
        strncpy(base, cfg->log_file, sizeof(base) - 1);

        /* Split directory and basename */
        char *slash = strrchr(dir, '/');
        const char *basename_ptr;
        if (slash) {
            *slash = '\0';
            basename_ptr = slash + 1;
        } else {
            strncpy(dir, ".", sizeof(dir) - 1);
            basename_ptr = base;
        }

        snprintf(prefix, sizeof(prefix), "%s.", basename_ptr);
        size_t prefix_len = strlen(prefix);

        /* Gather matching filenames */
        DIR *dp = opendir(dir);
        if (dp) {
            /* Simple insertion sort into a small fixed array */
            char entries[256][MAX_PATH];
            int  count = 0;

            struct dirent *ent;
            while ((ent = readdir(dp)) != NULL && count < 256) {
                if (strncmp(ent->d_name, prefix, prefix_len) == 0) {
                    snprintf(entries[count], MAX_PATH, "%s/%s", dir, ent->d_name);
                    count++;
                }
            }
            closedir(dp);

            /* Sort by name (lexicographic = chronological for ISO timestamps) */
            int i, j;
            for (i = 1; i < count; i++) {
                char tmp[MAX_PATH];
                strncpy(tmp, entries[i], MAX_PATH - 1);
                j = i - 1;
                while (j >= 0 && strcmp(entries[j], tmp) > 0) {
                    memcpy(entries[j + 1], entries[j], MAX_PATH);
                    j--;
                }
                strncpy(entries[j + 1], tmp, MAX_PATH - 1);
            }

            /* Remove oldest files beyond keep_count */
            int to_remove = count - cfg->keep_count;
            for (i = 0; i < to_remove && i < count; i++) {
                if (unlink(entries[i]) < 0) {
                    fprintf(stderr, "yap-rotate: unlink(%s): %s\n",
                            entries[i], strerror(errno));
                } else {
                    printf("yap-rotate: removed old log %s\n", entries[i]);
                }
            }
        }
    }

    return 0;
}

/* -------------------------------------------------------------------------
 * Usage / main
 * ---------------------------------------------------------------------- */

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s [-f] [-c config]\n"
            "  -f        force rotation regardless of thresholds\n"
            "  -c FILE   configuration file (default: %s)\n"
            "  -h        show this help\n",
            prog, YAP_CONFIG);
}

int main(int argc, char *argv[])
{
    int force = 0;
    const char *config_path = YAP_CONFIG;
    int opt;

    while ((opt = getopt(argc, argv, "fc:h")) != -1) {
        switch (opt) {
        case 'f': force = 1;         break;
        case 'c': config_path = optarg; break;
        case 'h': usage(argv[0]);    return EXIT_SUCCESS;
        default:  usage(argv[0]);    return EXIT_FAILURE;
        }
    }

    struct rotate_config cfg;
    if (config_load(config_path, &cfg) < 0) {
        fprintf(stderr, "yap-rotate: using default configuration\n");
        config_defaults(&cfg);
    }

    if (!cfg.enabled && !force) {
        printf("yap-rotate: rotation disabled in config — Time to tidy up later!\n");
        return EXIT_SUCCESS;
    }

    if (force || rotation_needed(&cfg)) {
        printf("yap-rotate: Time to tidy up the backyard! Rotating %s\n", cfg.log_file);
        if (do_rotate(&cfg) < 0) {
            fprintf(stderr, "yap-rotate: rotation failed\n");
            return EXIT_FAILURE;
        }
        printf("yap-rotate: rotation complete — all tidied up!\n");
    } else {
        printf("yap-rotate: no rotation needed — backyard is still tidy!\n");
    }

    return EXIT_SUCCESS;
}
