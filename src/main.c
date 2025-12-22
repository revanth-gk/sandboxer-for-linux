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

#define STACK_SIZE 1024 * 1024
#define SANDBOX_ROOT "/tmp/sandbox_root"

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

int setup_sandbox(void *arg) {
    struct SandboxConfig *config = (struct SandboxConfig *)arg;
    int run_shell = config ? config->memory : 0; // hack, use memory as flag
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

    // Set resource limits
    if (config) {
        struct rlimit rl;
        rl.rlim_cur = rl.rlim_max = config->memory * 1024 * 1024; // MB to bytes
        setrlimit(RLIMIT_AS, &rl);

        rl.rlim_cur = rl.rlim_max = config->cpu; // seconds
        setrlimit(RLIMIT_CPU, &rl);
    }

    if (run_shell) {
        // Run shell
        execl("/bin/busybox", "busybox", "sh", NULL);
        execl("/bin/sh", "sh", NULL);
        perror("execl");
        return 1;
    }
    return 0;
}

void create_sandbox(int memory, int cpu, int network, char *name) {
    log_action("Creating sandbox");

    // Prepare root dir
    mkdir(SANDBOX_ROOT, 0755);

    // Mount tmpfs
    if (mount("tmpfs", SANDBOX_ROOT, "tmpfs", 0, NULL) == -1) {
        perror("mount tmpfs");
        return;
    }

    // Create bin dir
    mkdir(SANDBOX_ROOT "/bin", 0755);

    // Copy busybox
    char cmd[256];
    sprintf(cmd, "cp /bin/busybox %s/bin/ 2>/dev/null || true", SANDBOX_ROOT);
    (void)system(cmd);

    int flags = CLONE_NEWPID | CLONE_NEWUSER | CLONE_NEWNS | SIGCHLD;
    if (!network) {
        flags |= CLONE_NEWNET;
    }

    struct SandboxConfig config = {memory, cpu, network};
    pid_t pid = clone(setup_sandbox, child_stack + STACK_SIZE, flags, &config);
    if (pid == -1) {
        perror("clone");
        return;
    }

    // Map uid/gid for user namespace
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

    waitpid(pid, NULL, 0);
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
}

void enter_sandbox(char *name) {
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

    // If already created, just clone and enter
    pid_t pid = clone(setup_sandbox, child_stack + STACK_SIZE,
                      CLONE_NEWPID | CLONE_NEWUSER | (config.network ? 0 : CLONE_NEWNET) | CLONE_NEWNS | SIGCHLD,
                      &config);
    if (pid == -1) {
        perror("clone");
        return;
    }

    // Map uid/gid for user namespace
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

    waitpid(pid, NULL, 0);
    log_action("Entered sandbox");
}

void delete_sandbox() {
    log_action("Deleting sandbox");
    char cmd[256];
    sprintf(cmd, "umount %s 2>/dev/null || true", SANDBOX_ROOT);
    (void)system(cmd);
    rmdir(SANDBOX_ROOT);
}

int main(int argc, char *argv[]) {
    int opt;
    int memory = 100; // MB
    int cpu = 10; // sec
    int network = 0; // 0 disable, 1 enable
    char *name = NULL;
    while ((opt = getopt(argc, argv, "cedm:t:ns:")) != -1) {
        switch (opt) {
            case 'c':
                create_sandbox(memory, cpu, network, name);
                break;
            case 'e':
                enter_sandbox(name);
                break;
            case 'd':
                delete_sandbox();
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
    return 0;
}