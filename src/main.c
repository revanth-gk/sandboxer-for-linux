#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sched.h>
#include <limits.h>
#include <getopt.h>
#include <sys/resource.h>
#include <stdint.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <errno.h>

#define STACK_SIZE 1024 * 1024
#define SANDBOX_ROOT "/tmp/sandbox_root"
#define MAX_CMD 512

static char child_stack[STACK_SIZE];

struct SandboxConfig {
    int memory; // MB
    int cpu;    // sec
    int network; // 0 disable, 1 enable
};

void log_action(const char *action) {
    FILE *log = fopen("/tmp/sandbox.log", "a");
    if (log) {
        fprintf(log, "%s\n", action);
        fclose(log);
    }
}

static void ensure_dns(void) {
    struct stat st;
    if (stat("/etc/resolv.conf", &st) == -1 || st.st_size == 0) {
        FILE *f = fopen("/etc/resolv.conf", "w");
        if (f) {
            fputs("nameserver 8.8.8.8\n", f);
            fclose(f);
            log_action("Wrote default DNS to /etc/resolv.conf");
        } else {
            perror("fopen /etc/resolv.conf");
        }
    }
}

static void enable_ip_forward(void) {
    int rc = system("sysctl -w net.ipv4.ip_forward=1 >/dev/null 2>&1");
    if (rc != 0) {
        log_action("Failed to enable ip_forward");
    } else {
        log_action("Enabled ip_forward");
    }
}

static void setup_nat_rules(void) {
    const char *cmds[] = {
        "iptables --table nat -A POSTROUTING -o eth0 -j MASQUERADE",
        "iptables -A FORWARD -i eth0 -o eth0 -m state --state RELATED,ESTABLISHED -j ACCEPT",
        "iptables -A FORWARD -i eth0 -o eth0 -j ACCEPT",
        NULL
    };
    for (int i = 0; cmds[i]; ++i) {
        int rc = system(cmds[i]);
        if (rc != 0) {
            log_action("Failed to apply NAT rule");
        }
    }
}

static void ensure_file(const char *path) {
    int fd = open(path, O_CREAT | O_RDONLY, 0644);
    if (fd >= 0) close(fd);
}

static int mkdir_p(const char *path, mode_t mode) {
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s", path);
    size_t len = strlen(tmp);
    if (len == 0) return -1;
    if (tmp[len - 1] == '/') tmp[len - 1] = '\0';
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, mode);
            *p = '/';
        }
    }
    return mkdir(tmp, mode);
}

static void install_host_packages(void) {
    const char *cmd = "apt-get update && apt-get install -y iptables net-tools dnsutils sudo iproute2 curl wget";
    int rc = system(cmd);
    if (rc != 0) {
        log_action("Package install failed");
    } else {
        log_action("Package install succeeded");
    }
}

static void bind_host_tools(void) {
    const char *dirs[] = {"/bin", "/usr/bin", "/usr/sbin", "/lib", "/lib64", "/usr/lib", "/usr/libexec", "/usr/lib/sudo", "/usr/libexec/sudo", NULL};
    char target[MAX_CMD];
    for (int i = 0; dirs[i]; ++i) {
        snprintf(target, sizeof(target), SANDBOX_ROOT "%s", dirs[i]);
        mkdir_p(target, 0755);
        char cmd[MAX_CMD];
        snprintf(cmd, sizeof(cmd), "mount --bind %s %s", dirs[i], target);
        (void)system(cmd);
    }
    // bind resolv.conf if present
    struct stat st;
    if (stat("/etc/resolv.conf", &st) == 0) {
        mkdir(SANDBOX_ROOT "/etc", 0755);
        ensure_file(SANDBOX_ROOT "/etc/resolv.conf");
        char cmd[MAX_CMD];
        snprintf(cmd, sizeof(cmd), "mount --bind /etc/resolv.conf %s", SANDBOX_ROOT "/etc/resolv.conf");
        (void)system(cmd);
    }
    // bind ld cache and configs
    if (stat("/etc/ld.so.cache", &st) == 0) {
        ensure_file(SANDBOX_ROOT "/etc/ld.so.cache");
        char cmd[MAX_CMD];
        snprintf(cmd, sizeof(cmd), "mount --bind /etc/ld.so.cache %s", SANDBOX_ROOT "/etc/ld.so.cache");
        (void)system(cmd);
    }
    mkdir_p(SANDBOX_ROOT "/etc/ld.so.conf.d", 0755);
    ensure_file(SANDBOX_ROOT "/etc/ld.so.conf");
    (void)system("mount --bind /etc/ld.so.conf " SANDBOX_ROOT "/etc/ld.so.conf");
    (void)system("mount --bind /etc/ld.so.conf.d " SANDBOX_ROOT "/etc/ld.so.conf.d");
    // sudo/pam/passwd
    ensure_file(SANDBOX_ROOT "/etc/sudoers");
    (void)system("mount --bind /etc/sudoers " SANDBOX_ROOT "/etc/sudoers");
    mkdir_p(SANDBOX_ROOT "/etc/pam.d", 0755);
    (void)system("mount --bind /etc/pam.d " SANDBOX_ROOT "/etc/pam.d");
    mkdir_p(SANDBOX_ROOT "/etc/security", 0755);
    (void)system("mount --bind /etc/security " SANDBOX_ROOT "/etc/security");
    ensure_file(SANDBOX_ROOT "/etc/nsswitch.conf");
    (void)system("mount --bind /etc/nsswitch.conf " SANDBOX_ROOT "/etc/nsswitch.conf");
    ensure_file(SANDBOX_ROOT "/etc/login.defs");
    (void)system("mount --bind /etc/login.defs " SANDBOX_ROOT "/etc/login.defs");
    ensure_file(SANDBOX_ROOT "/etc/passwd");
    (void)system("mount --bind /etc/passwd " SANDBOX_ROOT "/etc/passwd");
    ensure_file(SANDBOX_ROOT "/etc/group");
    (void)system("mount --bind /etc/group " SANDBOX_ROOT "/etc/group");
    ensure_file(SANDBOX_ROOT "/etc/shadow");
    (void)system("mount --bind /etc/shadow " SANDBOX_ROOT "/etc/shadow");
    mkdir_p(SANDBOX_ROOT "/var/run/sudo", 0700);
    mkdir_p(SANDBOX_ROOT "/var/lib/sudo", 0700);
    // bind SSL certificates
    mkdir_p(SANDBOX_ROOT "/etc/ssl", 0755);
    (void)system("mount --bind /etc/ssl " SANDBOX_ROOT "/etc/ssl");
    mkdir_p(SANDBOX_ROOT "/usr/share/ca-certificates", 0755);
    (void)system("mount --bind /usr/share/ca-certificates " SANDBOX_ROOT "/usr/share/ca-certificates");
    mkdir_p(SANDBOX_ROOT "/etc/ca-certificates", 0755);
    (void)system("mount --bind /etc/ca-certificates " SANDBOX_ROOT "/etc/ca-certificates");
}

int setup_sandbox(void *arg) {
    struct SandboxConfig *config = (struct SandboxConfig *)arg;
    int should_run_shell = config ? 1 : 0; // Always run shell when config is provided
    log_action("Setting up sandbox");

    // Chroot
    if (chroot(SANDBOX_ROOT) == -1) {
        perror("chroot");
        return 1;
    }

    if (chdir("/") == -1) {
        perror("chdir");
        return 1;
    }

    // Create directories
    mkdir("bin", 0755);
    mkdir("proc", 0755);
    mkdir("tmp", 0755);
    mkdir("dev", 0755);
    mkdir("etc", 0755);

    // Mount proc
    if (mount("proc", "/proc", "proc", 0, NULL) == -1) {
        perror("mount proc");
        return 1;
    }

    // Mount dev
    if (mount("tmpfs", "/dev", "tmpfs", 0, NULL) == -1) {
        perror("mount dev");
        return 1;
    }
    // Minimal device nodes
    mknod("/dev/null", S_IFCHR | 0666, makedev(1, 3));
    mknod("/dev/zero", S_IFCHR | 0666, makedev(1, 5));
    mknod("/dev/random", S_IFCHR | 0666, makedev(1, 8));
    mknod("/dev/urandom", S_IFCHR | 0666, makedev(1, 9));
    mknod("/dev/tty", S_IFCHR | 0666, makedev(5, 0));

    // Set resource limits
    if (config) {
        struct rlimit rl;
        rl.rlim_cur = config->memory * 1024 * 1024; // MB to bytes
        rl.rlim_max = RLIM_INFINITY; // Allow increasing soft limit
        if (setrlimit(RLIMIT_AS, &rl) == -1) {
            perror("setrlimit RLIMIT_AS");
        }

        rl.rlim_cur = config->cpu; // seconds
        rl.rlim_max = RLIM_INFINITY; // Allow increasing soft limit
        if (setrlimit(RLIMIT_CPU, &rl) == -1) {
            perror("setrlimit RLIMIT_CPU");
        }
    }

    if (should_run_shell) {
        // Ensure resolv.conf inside chroot
        FILE *rf = fopen("/etc/resolv.conf", "w");
        if (rf) {
            fputs("nameserver 8.8.8.8\nnameserver 8.8.4.4\n", rf);
            fclose(rf);
        }
        // Run shell
        execl("/bin/busybox", "busybox", "sh", NULL);
        execl("/bin/sh", "sh", NULL);
        perror("execl");
        return 1;
    }
    return 0;
}

int create_sandbox(int memory, int cpu, int network, char *name) {
    log_action("Creating sandbox");
    int rc = 0;

    // Prepare root dir
    mkdir(SANDBOX_ROOT, 0755);

    // Mount tmpfs
    if (mount("tmpfs", SANDBOX_ROOT, "tmpfs", 0, NULL) == -1) {
        perror("mount tmpfs");
        return 1;
    }

    // Create initial dirs
    mkdir_p(SANDBOX_ROOT "/bin", 0755);
    mkdir_p(SANDBOX_ROOT "/usr/bin", 0755);
    mkdir_p(SANDBOX_ROOT "/usr/sbin", 0755);
    mkdir_p(SANDBOX_ROOT "/lib", 0755);
    mkdir_p(SANDBOX_ROOT "/lib64", 0755);
    mkdir_p(SANDBOX_ROOT "/usr/lib", 0755);

    // Copy busybox
    char cmd[256];
    sprintf(cmd, "cp /bin/busybox %s/bin/ 2>/dev/null || true", SANDBOX_ROOT);
    (void)system(cmd);

    if (network) {
        if (getuid() != 0) {
            fprintf(stderr, "Error: networked sandboxes require root (for iptables/sysctl).\n");
            return 1;
        }
        ensure_dns();
        enable_ip_forward();
        setup_nat_rules();
        install_host_packages();
        bind_host_tools();
    }

    int flags = CLONE_NEWPID | CLONE_NEWNS | SIGCHLD;
    if (!network) {
        flags |= CLONE_NEWUSER | CLONE_NEWNET; // isolate when no network
    }

    struct SandboxConfig config = {memory, cpu, network};
    pid_t pid = clone(setup_sandbox, child_stack + STACK_SIZE, flags, &config);
    if (pid == -1) {
        perror("clone");
        return 1;
    }

    // Map uid/gid for user namespace
    if (flags & CLONE_NEWUSER) {
        char path[256];
        sprintf(path, "/proc/%d/uid_map", pid);
        FILE *f = fopen(path, "w");
        if (f) {
            fprintf(f, "0 %d 1\n", getuid());
            fclose(f);
        }
        sprintf(path, "/proc/%d/gid_map", pid);
        f = fopen(path, "w");
        if (f) {
            fprintf(f, "0 %d 1\n", getgid());
            fclose(f);
        }
    }

    if (waitpid(pid, NULL, 0) == -1) {
        perror("waitpid");
        rc = 1;
    }
    log_action("Sandbox created");

    // Save config
    if (name) {
        FILE *config_file = fopen("sandboxes.txt", "a");
        if (config_file) {
            time_t now = time(NULL);
            fprintf(config_file, "%s %d %d %d %ld\n", name, memory, cpu, network, now);
            fclose(config_file);
        }
    }
    return rc;
}

int enter_sandbox(char *name) {
    log_action("Entering sandbox");

    struct SandboxConfig config = {100, 10, 0}; // default
    if (name) {
        FILE *f = fopen("sandboxes.txt", "r");
        if (f) {
            char line[512];
            while (fgets(line, sizeof(line), f)) {
                char n[256];
                int m, c, net;
                long t;
                if (sscanf(line, "%s %d %d %d %ld", n, &m, &c, &net, &t) == 5 && strcmp(n, name) == 0) {
                    config.memory = m;
                    config.cpu = c;
                    config.network = net;
                    break;
                }
            }
            fclose(f);
        }
    }

    if (config.network) {
        if (getuid() != 0) {
            fprintf(stderr, "Error: networked sandboxes require root (for iptables/sysctl).\n");
            return 1;
        }
        ensure_dns();
        enable_ip_forward();
        setup_nat_rules();
        install_host_packages();
        bind_host_tools();
    }

    int flags = CLONE_NEWPID | CLONE_NEWNS | SIGCHLD;
    if (!config.network) {
        flags |= CLONE_NEWUSER | CLONE_NEWNET;
    }

    // If already created, just clone and enter
    pid_t pid = clone(setup_sandbox, child_stack + STACK_SIZE, flags, &config);
    if (pid == -1) {
        perror("clone");
        return 1;
    }

    // Map uid/gid for user namespace
    if (flags & CLONE_NEWUSER) {
        char path[256];
        sprintf(path, "/proc/%d/uid_map", pid);
        FILE *f = fopen(path, "w");
        if (f) {
            fprintf(f, "0 %d 1\n", getuid());
            fclose(f);
        }
        sprintf(path, "/proc/%d/gid_map", pid);
        f = fopen(path, "w");
        if (f) {
            fprintf(f, "0 %d 1\n", getgid());
            fclose(f);
        }
    }

    if (waitpid(pid, NULL, 0) == -1) {
        perror("waitpid");
        return 1;
    }
    log_action("Entered sandbox");
    return 0;
}

int delete_sandbox() {
    log_action("Deleting sandbox");
    char cmd[256];
    sprintf(cmd, "umount %s 2>/dev/null || true", SANDBOX_ROOT);
    (void)system(cmd);
    rmdir(SANDBOX_ROOT);
    return 0;
}

int main(int argc, char *argv[]) {
    int opt;
    int memory = 100; // MB
    int cpu = 10; // sec
    int network = 0; // 0 disable, 1 enable
    int create = 0, enter = 0, delete = 0;
    char *name = NULL;
    
    while ((opt = getopt(argc, argv, "cedm:t:ns:")) != -1) {
        switch (opt) {
            case 'c':
                create = 1;
                break;
            case 'e':
                enter = 1;
                break;
            case 'd':
                delete = 1;
                break;
            case 'm':
                memory = atoi(optarg);
                break;
            case 't':
                cpu = atoi(optarg);
                break;
            case 'n':
                network = 1;
                break;
            case 's':
                name = optarg;
                break;
            default:
                fprintf(stderr, "Usage: %s -c (create) -e (enter) -d (delete) [-m memory(MB)] [-t cpu(sec)] [-n (enable network)] [-s name]\n", argv[0]);
                return 1;
        }
    }
    
    // Validate mutually exclusive options
    int action_count = create + enter + delete;
    if (action_count == 0) {
        fprintf(stderr, "Error: Must specify one of -c, -e, or -d\n");
        fprintf(stderr, "Usage: %s -c (create) -e (enter) -d (delete) [-m memory(MB)] [-t cpu(sec)] [-n (enable network)] [-s name]\n", argv[0]);
        return 1;
    }
    
    if (action_count > 1) {
        fprintf(stderr, "Error: Cannot specify more than one of -c, -e, or -d\n");
        return 1;
    }
    
    int rc = 0;
    if (create) {
        rc = create_sandbox(memory, cpu, network, name);
    } else if (enter) {
        rc = enter_sandbox(name);
    } else if (delete) {
        rc = delete_sandbox();
    }
    
    return rc;
}