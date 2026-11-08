#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define BLKROSET   0x125d
#define KSUD_STAGE "/data/user_de/0/df.root/ksud"
#define KSUD       "/data/adb/ksud"
#define PREFS_PATH "/data/user_de/0/df.root/shared_prefs/dfroot.xml"
#define MODULES_DIR "/data/adb/modules"
/* One line, written by this process and read back by the app after a run; see
 * soft_reboot_report(). The app's own DE dir, world-readable once root puts a
 * file in it - the same directory the staged ksud above sits in. */
#define SR_REPORT  "/data/user_de/0/df.root/soft_reboot_status"

static int pref_true(const char *buf, const char *key)
{
    char needle[64];
    snprintf(needle, sizeof(needle), "name=\"%s\"", key);
    char *p = strstr(buf, needle);
    if (!p) return 0;
    char *tag_end = strchr(p, '>');
    char *v = strstr(p, "value=\"true\"");
    return v && tag_end && v < tag_end;
}

static int read_prefs(char *su_manager, size_t su_manager_size, int *soft_reboot,
                      int *disable_modules)
{
    int fd = open(PREFS_PATH, O_RDONLY);
    if (fd < 0) return -1;

    /* Read the whole file, not a leading slice of it. SharedPreferences writes
     * its keys in HashMap order, so which key lands past the end of a short
     * buffer is the hash function's choice, not ours - and a soft_reboot that
     * sits past the end reads exactly like a soft_reboot that was never turned
     * on. The persisted manager list the picker now keeps is what pushed this
     * file past the old 4 KB window. */
    static char buf[65536];
    int n = 0;
    while (n < (int)sizeof(buf) - 1) {
        int r = read(fd, buf + n, sizeof(buf) - 1 - n);
        if (r <= 0) break;
        n += r;
    }
    close(fd);
    if (n <= 0) return -1;
    buf[n] = '\0';

    char *p = strstr(buf, "name=\"su_manager\">");
    if (!p) return -1;
    p += strlen("name=\"su_manager\">");
    char *end = strchr(p, '<');
    if (!end) return -1;
    size_t len = end - p;
    if (len == 0 || len >= su_manager_size) return -1;
    memcpy(su_manager, p, len);
    su_manager[len] = '\0';

    *soft_reboot = pref_true(buf, "soft_reboot");
    *disable_modules = pref_true(buf, "disable_modules");

    return 0;
}

static int adopt_zygote_env(void)
{
    FILE *f = popen("pidof zygote64 zygote", "r");
    if (!f) return -1;
    int pid = 0;
    fscanf(f, "%d", &pid);
    pclose(f);
    if (!pid) return -1;

    char path[32];
    snprintf(path, sizeof(path), "/proc/%d/environ", pid);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    static char buf[16384];
    int n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return -1;
    buf[n] = '\0';
    for (char *p = buf, *end = buf + n; p < end; p += strlen(p) + 1)
        putenv(p);
    return 0;
}

static int should_ro(const char *name)
{
    size_t len = strlen(name);
    if (!strcmp(name, "super"))  return 1;
    if (!strcmp(name, "misc"))   return 1;
    if (!strcmp(name, "steady")) return 1;
    if (len >= 2 && name[len - 2] == '_' &&
        (name[len - 1] == 'a' || name[len - 1] == 'b'))
        return 1;
    return 0;
}

static int set_partitions_ro(void)
{
    DIR *dir = opendir("/dev/block/by-name");
    if (!dir)
        return -1;

    struct dirent *ent;
    while ((ent = readdir(dir))) {
        if (!should_ro(ent->d_name))
            continue;

        char path[128];
        snprintf(path, sizeof(path), "/dev/block/by-name/%s", ent->d_name);

        int fd = open(path, O_RDONLY);
        if (fd < 0)
            continue;

        struct stat st;
        if (fstat(fd, &st) == 0 && S_ISBLK(st.st_mode)) {
            int on = 1;
            ioctl(fd, BLKROSET, &on);
        }
        close(fd);
    }

    closedir(dir);
    return 0;
}

static int late_load_running(void)
{
    DIR *d = opendir("/proc");
    if (!d)
        return 0;
    struct dirent *e;
    char path[64], buf[256];
    int found = 0;
    while ((e = readdir(d)) != NULL) {
        char *end;
        strtol(e->d_name, &end, 10);
        if (*end) continue;
        snprintf(path, sizeof(path), "/proc/%s/cmdline", e->d_name);
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) continue;
        int n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n <= 0) continue;
        buf[n] = '\0';
        for (int i = 0; i < n; i++)
            if (!buf[i]) buf[i] = ' ';
        if (strstr(buf, "ksud") && strstr(buf, "late-load")) {
            found = 1;
            break;
        }
    }
    closedir(d);
    return found;
}

static int run_ctx(const char *ctx, char *const argv[])
{
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        if (ctx) {
            int fd = open("/proc/self/attr/exec", O_WRONLY);
            if (fd >= 0) { write(fd, ctx, strlen(ctx)); close(fd); }
        }
        execv(argv[0], argv);
        _exit(127);
    }
    int status;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static void touch(const char *path)
{
    int fd = open(path, O_CREAT | O_WRONLY, 0666);
    if (fd >= 0)
        close(fd);
}

/* Which ksud can be asked for a soft reboot.
 *
 * The manager's own userspace is the only thing allowed to reboot this device:
 * a soft reboot restarts Android's userspace and leaves the kernel - and the
 * page-cache patch this root lives in - alone, which is what the Auto reboot
 * switch promises. A plain `setprop sys.powerctl reboot` would be a FULL
 * reboot and drop the root, so nothing here may fall back to it; the choice on
 * offer is only which ksud to ask.
 *
 * The old code named /data/adb/ksud and nothing else. That is the path a
 * *flashed* KernelSU installs, and in this flow the phone is not flashed and
 * had no root to install anything with - the staged copy above exists precisely
 * because the manager's userspace is not on the path. On such a device the exec
 * failed with 127, run_ctx's return was discarded, and the owner saw a root
 * that simply never rebooted with nothing in the log to explain it. The staged
 * binary is the one this flow knows is real, so it is now the last resort
 * instead of the only candidate. */
static const char *soft_reboot_ksud(void)
{
    static const char *const cands[] = {
        KSUD,
        "/data/adb/ksu/bin/ksud",
        KSUD_STAGE,
    };
    size_t i;
    for (i = 0; i < sizeof(cands) / sizeof(cands[0]); i++)
        if (access(cands[i], X_OK) == 0)
            return cands[i];
    return NULL;
}

/* The verdict above, one line, for the app to read back. Written BEFORE the
 * attempt as well as after a failed one: the reboot a successful attempt
 * triggers takes this process - and the app reading over its shoulder - with
 * it, so an accepted request still needs to be on disk. */
static void soft_reboot_report(const char *text)
{
    int fd = open(SR_REPORT, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (fd < 0)
        return;
    if (write(fd, text, strlen(text)) < 0) {
        /* Nothing to do about it; the file is a courtesy, not the mechanism. */
    }
    close(fd);
}

/* Mark every installed module disabled before ksud runs. A broken module
 * otherwise loads on the next boot and bootloops the device. */
static int disable_modules(void)
{
    DIR *dir = opendir(MODULES_DIR);
    if (!dir)
        return -1;

    struct dirent *ent;
    while ((ent = readdir(dir))) {
        if (ent->d_name[0] == '.')
            continue;
        char path[256];
        snprintf(path, sizeof(path), MODULES_DIR "/%s/disable", ent->d_name);
        touch(path);
    }
    closedir(dir);
    return 0;
}

int main(void)
{
    touch("/dev/dfm1");
    char su_manager[256];
    int soft_reboot, disable_mods;
    if (read_prefs(su_manager, sizeof(su_manager), &soft_reboot, &disable_mods) != 0) {
        touch("/dev/dfme0");
        return 1;
    }

    touch("/dev/dfm2");
    if (adopt_zygote_env())
        touch("/dev/dfmw0");

    touch("/dev/dfm3");
    if (set_partitions_ro())
        touch("/dev/dfmw1");

    if (disable_mods) {
        touch("/dev/dfm4");
        if (disable_modules()) {
            touch("/dev/dfme1");
            return 1;
        }
    }

    touch("/dev/dfm5");    
    if (run_ctx(NULL, (char *[]){ KSUD_STAGE, "late-load", "--package-name", su_manager, NULL }) == 0) {
        touch("/dev/dfm6");
        if (soft_reboot) {
            char line[256];
            const char *ksud;
            int rc, waited = 0;

            /* late-load is expected to exit on its own, and the reboot has to
             * wait for it: rebooting under a daemon that is still loading its
             * modules loses them. But a resident ksud that still carries its
             * original cmdline matched the old unbounded wait, and that waited
             * out the phone rather than the module load. */
            while (late_load_running() && waited++ < 30)
                sleep(1);

            ksud = soft_reboot_ksud();
            if (!ksud) {
                snprintf(line, sizeof(line),
                         "failed: no ksud to ask - tried %s, /data/adb/ksu/bin/ksud and %s",
                         KSUD, KSUD_STAGE);
                soft_reboot_report(line);
            } else {
                snprintf(line, sizeof(line), "attempted: %s", ksud);
                soft_reboot_report(line);
                rc = run_ctx("u:r:ksu:s0",
                             (char *[]){ (char *)ksud, "soft-reboot", NULL });
                if (rc != 0) {
                    /* 127 is the exec itself failing: present, executable, and
                     * still not something this kernel will run. */
                    snprintf(line, sizeof(line),
                             "failed: %s soft-reboot returned %d", ksud, rc);
                    soft_reboot_report(line);
                }
            }
        }
    } else {
        touch("/dev/dfme2");
    }
        
    return 0;
}
