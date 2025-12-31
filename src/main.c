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
#include <dirent.h>

#define STACK_SIZE 1024 * 1024
#define SANDBOX_ROOT "/tmp/sandbox_root"
#define MAX_CMD 1024

static char child_stack[STACK_SIZE];

struct SandboxConfig {
    int memory;     // MB - memory limit
    int cpu_cores;  // Number of CPU cores to allow (0 = no limit)
    int network;    // 0 disable, 1 enable
};

void log_action(const char *action) {
    FILE *log = fopen("/tmp/sandbox.log", "a");
    if (log) {
        fprintf(log, "%s\n", action);
        fclose(log);
    }
}

// Check system requirements and print helpful messages
static int check_system_requirements(void) {
    int ok = 1;
    struct stat st;
    
    // Check if we're on Linux
    #ifndef __linux__
    fprintf(stderr, "Error: This program only works on Linux.\n");
    return 0;
    #endif
    
    // Check for user namespace support
    FILE *f = fopen("/proc/sys/kernel/unprivileged_userns_clone", "r");
    if (f) {
        int val = 0;
        if (fscanf(f, "%d", &val) == 1 && val == 0) {
            fprintf(stderr, "Warning: Unprivileged user namespaces are disabled.\n");
            fprintf(stderr, "  Run: sudo sysctl -w kernel.unprivileged_userns_clone=1\n");
            fprintf(stderr, "  Or run this program as root.\n");
            // Not a fatal error if running as root
            if (getuid() != 0) {
                ok = 0;
            }
        }
        fclose(f);
    }
    
    // Check for at least one shell
    const char *shells[] = {
        "/bin/busybox", "/bin/bash", "/bin/sh", "/bin/dash", "/bin/zsh",
        "/usr/bin/bash", "/usr/bin/sh", NULL
    };
    int shell_found = 0;
    for (int i = 0; shells[i]; i++) {
        if (stat(shells[i], &st) == 0) {
            shell_found = 1;
            break;
        }
    }
    if (!shell_found) {
        fprintf(stderr, "Warning: No shell found (busybox, bash, sh, dash, zsh).\n");
        fprintf(stderr, "  Install one with: sudo apt install busybox-static\n");
        fprintf(stderr, "  Or: sudo apt install bash\n");
    }
    
    // Check if /tmp is writable
    if (access("/tmp", W_OK) != 0) {
        fprintf(stderr, "Error: /tmp is not writable.\n");
        ok = 0;
    }
    
    return ok;
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

// Get number of available CPU cores
static int get_cpu_count(void) {
    int count = sysconf(_SC_NPROCESSORS_ONLN);
    return count > 0 ? count : 1;
}

// Set CPU affinity to limit cores (call from child process)
static void apply_cpu_limit(int max_cores) {
    if (max_cores <= 0) return;
    
    int total_cores = get_cpu_count();
    if (max_cores >= total_cores) return; // No limit needed
    
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    
    // Allow cores 0 to max_cores-1
    for (int i = 0; i < max_cores && i < total_cores; i++) {
        CPU_SET(i, &cpuset);
    }
    
    if (sched_setaffinity(0, sizeof(cpuset), &cpuset) == -1) {
        fprintf(stderr, "Warning: Could not set CPU affinity: %s\n", strerror(errno));
    } else {
        char msg[128];
        snprintf(msg, sizeof(msg), "CPU limited to %d core(s)", max_cores);
        log_action(msg);
    }
}

// Apply memory limit using cgroups v2 (if available) or rlimit
static void apply_memory_limit(int memory_mb) {
    if (memory_mb <= 0) return;
    
    // Try cgroups v2 first
    char cgroup_path[PATH_MAX];
    pid_t pid = getpid();
    snprintf(cgroup_path, sizeof(cgroup_path), "/sys/fs/cgroup/sandbox_%d", pid);
    
    if (mkdir(cgroup_path, 0755) == 0 || errno == EEXIST) {
        char mem_max_path[PATH_MAX];
        snprintf(mem_max_path, sizeof(mem_max_path), "%s/memory.max", cgroup_path);
        
        FILE *f = fopen(mem_max_path, "w");
        if (f) {
            fprintf(f, "%ldM\n", (long)memory_mb);
            fclose(f);
            
            // Add current process to cgroup
            char procs_path[PATH_MAX];
            snprintf(procs_path, sizeof(procs_path), "%s/cgroup.procs", cgroup_path);
            FILE *pf = fopen(procs_path, "w");
            if (pf) {
                fprintf(pf, "%d\n", pid);
                fclose(pf);
                log_action("Memory limit applied via cgroups v2");
                return;
            }
        }
    }
    
    // Fallback to rlimit (less accurate but works everywhere)
    struct rlimit rl;
    rl.rlim_cur = (rlim_t)memory_mb * 1024 * 1024;
    rl.rlim_max = (rlim_t)memory_mb * 1024 * 1024 * 2; // Hard limit 2x soft
    if (setrlimit(RLIMIT_AS, &rl) == -1) {
        fprintf(stderr, "Warning: Could not set memory limit: %s\n", strerror(errno));
    } else {
        log_action("Memory limit applied via rlimit");
    }
}

// Bind essential libraries for minimal sandbox functionality (non-network mode)
static void bind_essential_libs(void) {
    struct stat st;
    char cmd[MAX_CMD];
    
    log_action("Setting up essential libraries for isolated sandbox...");
    
    // Create ALL essential directories
    const char *essential_dirs[] = {
        SANDBOX_ROOT "/bin",
        SANDBOX_ROOT "/sbin",
        SANDBOX_ROOT "/usr/bin",
        SANDBOX_ROOT "/usr/sbin",
        SANDBOX_ROOT "/lib",
        SANDBOX_ROOT "/lib64",
        SANDBOX_ROOT "/lib/x86_64-linux-gnu",
        SANDBOX_ROOT "/usr/lib",
        SANDBOX_ROOT "/usr/lib/x86_64-linux-gnu",
        SANDBOX_ROOT "/etc",
        SANDBOX_ROOT "/tmp",
        SANDBOX_ROOT "/var",
        SANDBOX_ROOT "/var/tmp",
        SANDBOX_ROOT "/proc",
        SANDBOX_ROOT "/dev",
        NULL
    };
    for (int i = 0; essential_dirs[i]; ++i) {
        mkdir_p(essential_dirs[i], 0755);
    }
    
    // Copy essential dynamic linker
    const char *ld_paths[] = {
        "/lib64/ld-linux-x86-64.so.2",
        "/lib/ld-linux.so.2",
        "/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2",
        "/lib/ld-linux-x86-64.so.2",
        NULL
    };
    for (int i = 0; ld_paths[i]; ++i) {
        if (stat(ld_paths[i], &st) == 0) {
            // Ensure target directory exists
            char target_dir[MAX_CMD];
            snprintf(target_dir, sizeof(target_dir), "%s%s", SANDBOX_ROOT, ld_paths[i]);
            char *last_slash = strrchr(target_dir, '/');
            if (last_slash) {
                *last_slash = '\0';
                mkdir_p(target_dir, 0755);
            }
            snprintf(cmd, sizeof(cmd), "cp -L %s %s%s 2>/dev/null || true", 
                    ld_paths[i], SANDBOX_ROOT, ld_paths[i]);
            (void)system(cmd);
        }
    }
    
    // Copy essential C library files - including SELinux and PCRE
    const char *libc_paths[] = {
        // Core C library
        "/lib/x86_64-linux-gnu/libc.so.6",
        "/lib/x86_64-linux-gnu/libm.so.6",
        "/lib/x86_64-linux-gnu/libpthread.so.0",
        "/lib/x86_64-linux-gnu/libdl.so.2",
        "/lib/x86_64-linux-gnu/librt.so.1",
        "/lib/x86_64-linux-gnu/libresolv.so.2",
        "/lib/x86_64-linux-gnu/libnss_files.so.2",
        "/lib/x86_64-linux-gnu/libnss_dns.so.2",
        // Terminal/ncurses
        "/lib/x86_64-linux-gnu/libtinfo.so.6",
        "/lib/x86_64-linux-gnu/libncurses.so.6",
        "/lib/x86_64-linux-gnu/libncursesw.so.6",
        "/usr/lib/x86_64-linux-gnu/libtinfo.so.6",
        "/usr/lib/x86_64-linux-gnu/libncurses.so.6",
        "/usr/lib/x86_64-linux-gnu/libncursesw.so.6",
        // SELinux (required by ls, etc.)
        "/lib/x86_64-linux-gnu/libselinux.so.1",
        "/usr/lib/x86_64-linux-gnu/libselinux.so.1",
        // PCRE (required by libselinux)
        "/lib/x86_64-linux-gnu/libpcre.so.3",
        "/lib/x86_64-linux-gnu/libpcre2-8.so.0",
        "/usr/lib/x86_64-linux-gnu/libpcre.so.3",
        "/usr/lib/x86_64-linux-gnu/libpcre2-8.so.0",
        // Other commonly needed libs
        "/lib/x86_64-linux-gnu/libcap.so.2",
        "/lib/x86_64-linux-gnu/libattr.so.1",
        "/lib/x86_64-linux-gnu/libacl.so.1",
        "/lib/x86_64-linux-gnu/libgcc_s.so.1",
        // lib64 versions
        "/lib64/libc.so.6",
        "/lib64/libm.so.6",
        "/lib64/libpthread.so.0",
        "/lib64/libdl.so.2",
        "/lib64/libtinfo.so.6",
        "/lib64/libselinux.so.1",
        "/lib64/libpcre.so.3",
        "/lib64/libpcre2-8.so.0",
        NULL
    };
    for (int i = 0; libc_paths[i]; ++i) {
        if (stat(libc_paths[i], &st) == 0) {
            // Ensure target directory exists
            char target_dir[MAX_CMD];
            snprintf(target_dir, sizeof(target_dir), "%s%s", SANDBOX_ROOT, libc_paths[i]);
            char *last_slash = strrchr(target_dir, '/');
            if (last_slash) {
                *last_slash = '\0';
                mkdir_p(target_dir, 0755);
            }
            snprintf(cmd, sizeof(cmd), "cp -L %s %s%s 2>/dev/null || true", 
                    libc_paths[i], SANDBOX_ROOT, libc_paths[i]);
            (void)system(cmd);
        }
    }
    
    // Copy ld.so.cache for library resolution
    if (stat("/etc/ld.so.cache", &st) == 0) {
        snprintf(cmd, sizeof(cmd), "cp /etc/ld.so.cache %s/etc/ 2>/dev/null || true", SANDBOX_ROOT);
        (void)system(cmd);
    }
    
    // Copy ALL possible shells
    const char *shells[] = {
        "/bin/busybox",
        "/bin/sh",
        "/bin/bash",
        "/bin/dash",
        "/bin/zsh",
        "/usr/bin/sh",
        "/usr/bin/bash",
        "/usr/bin/dash",
        "/usr/bin/zsh",
        NULL
    };
    
    int shell_copied = 0;
    for (int i = 0; shells[i]; ++i) {
        if (stat(shells[i], &st) == 0) {
            // Ensure target directory exists
            char target_dir[MAX_CMD];
            snprintf(target_dir, sizeof(target_dir), "%s%s", SANDBOX_ROOT, shells[i]);
            char *last_slash = strrchr(target_dir, '/');
            if (last_slash) {
                *last_slash = '\0';
                mkdir_p(target_dir, 0755);
            }
            
            // Copy the shell
            snprintf(cmd, sizeof(cmd), "cp -L %s %s%s 2>/dev/null", shells[i], SANDBOX_ROOT, shells[i]);
            if (system(cmd) == 0) {
                shell_copied = 1;
                
                // Also copy dependencies of this shell using ldd
                snprintf(cmd, sizeof(cmd), 
                    "ldd %s 2>/dev/null | grep -o '/[^ ]*' | while read lib; do "
                    "mkdir -p %s$(dirname \"$lib\") 2>/dev/null; "
                    "cp -L \"$lib\" %s\"$lib\" 2>/dev/null; done || true", 
                    shells[i], SANDBOX_ROOT, SANDBOX_ROOT);
                (void)system(cmd);
            }
        }
    }
    
    // Copy basic utilities WITH their dependencies
    const char *utils[] = {
        // Core file utilities
        "/bin/ls", "/bin/cat", "/bin/echo", "/bin/pwd", "/bin/mkdir",
        "/bin/rm", "/bin/cp", "/bin/mv", "/bin/touch", "/bin/chmod",
        "/bin/chown", "/bin/ln", "/bin/readlink", "/bin/date", "/bin/sleep",
        "/bin/dd", "/bin/df", "/bin/du", "/bin/uname", "/bin/hostname",
        // Terminal utilities - IMPORTANT for clear command
        "/usr/bin/clear", "/usr/bin/reset", "/usr/bin/tput", "/usr/bin/tset",
        "/bin/stty",
        // TEXT EDITORS - ESSENTIAL for editing files
        "/usr/bin/nano", "/bin/nano",
        "/usr/bin/vim", "/usr/bin/vi", "/bin/vi", "/usr/bin/vim.basic", "/usr/bin/vim.tiny",
        "/usr/bin/less", "/usr/bin/more", "/bin/more",
        "/usr/bin/editor",  // Debian's default editor link
        // Text processing utilities
        "/usr/bin/grep", "/bin/grep", "/usr/bin/egrep", "/usr/bin/fgrep",
        "/usr/bin/sed", "/bin/sed",
        "/usr/bin/head", "/usr/bin/tail", "/usr/bin/wc", "/usr/bin/sort",
        "/usr/bin/cut", "/usr/bin/tr", "/usr/bin/awk", "/usr/bin/gawk",
        "/usr/bin/xargs", "/usr/bin/find", "/bin/find",
        "/usr/bin/file", "/usr/bin/stat",
        // User utilities
        "/usr/bin/env", "/usr/bin/id", "/usr/bin/whoami", "/usr/bin/groups",
        "/usr/bin/which", "/usr/bin/dirname", "/usr/bin/basename",
        "/usr/bin/realpath", "/usr/bin/readlink",
        // Process utilities
        "/bin/ps", "/usr/bin/ps", "/bin/kill", "/usr/bin/kill",
        "/usr/bin/pgrep", "/usr/bin/pkill",
        NULL
    };
    for (int i = 0; utils[i]; ++i) {
        if (stat(utils[i], &st) == 0) {
            char target_dir[MAX_CMD];
            snprintf(target_dir, sizeof(target_dir), "%s%s", SANDBOX_ROOT, utils[i]);
            char *last_slash = strrchr(target_dir, '/');
            if (last_slash) {
                *last_slash = '\0';
                mkdir_p(target_dir, 0755);
            }
            // Copy the binary
            snprintf(cmd, sizeof(cmd), "cp -L %s %s%s 2>/dev/null || true", 
                    utils[i], SANDBOX_ROOT, utils[i]);
            (void)system(cmd);
            
            // Copy its library dependencies
            snprintf(cmd, sizeof(cmd), 
                "ldd %s 2>/dev/null | grep -oE '/[^ ]+' | while read lib; do "
                "mkdir -p %s$(dirname \"$lib\") 2>/dev/null; "
                "cp -Ln \"$lib\" %s\"$lib\" 2>/dev/null; done || true", 
                utils[i], SANDBOX_ROOT, SANDBOX_ROOT);
            (void)system(cmd);
        }
    }
    
    // Copy terminfo database for clear, reset, etc. to work
    mkdir_p(SANDBOX_ROOT "/usr/share/terminfo", 0755);
    mkdir_p(SANDBOX_ROOT "/lib/terminfo", 0755);
    mkdir_p(SANDBOX_ROOT "/etc/terminfo", 0755);
    
    const char *terminfo_paths[] = {"/usr/share/terminfo", "/lib/terminfo", "/etc/terminfo", NULL};
    for (int i = 0; terminfo_paths[i]; ++i) {
        if (stat(terminfo_paths[i], &st) == 0) {
            snprintf(cmd, sizeof(cmd), "cp -rL %s/* %s/usr/share/terminfo/ 2>/dev/null || true", 
                    terminfo_paths[i], SANDBOX_ROOT);
            (void)system(cmd);
        }
    }
    
    // Copy /etc/passwd and /etc/group for user utilities
    snprintf(cmd, sizeof(cmd), "cp /etc/passwd %s/etc/ 2>/dev/null || true", SANDBOX_ROOT);
    (void)system(cmd);
    snprintf(cmd, sizeof(cmd), "cp /etc/group %s/etc/ 2>/dev/null || true", SANDBOX_ROOT);
    (void)system(cmd);
    
    // Create /etc/profile to set TERM and TERMINFO
    FILE *profile = fopen(SANDBOX_ROOT "/etc/profile", "w");
    if (profile) {
        fprintf(profile, "export TERM=${TERM:-xterm}\n");
        fprintf(profile, "export TERMINFO=/usr/share/terminfo\n");
        fprintf(profile, "export PATH=/bin:/usr/bin:/sbin:/usr/sbin\n");
        fprintf(profile, "export VIMRUNTIME=/usr/share/vim/vim*\n");
        fclose(profile);
    }
    
    // Copy vim configuration files - to fix "Failed to source defaults.vim"
    mkdir_p(SANDBOX_ROOT "/usr/share/vim", 0755);
    snprintf(cmd, sizeof(cmd), "cp -rL /usr/share/vim/* %s/usr/share/vim/ 2>/dev/null || true", SANDBOX_ROOT);
    (void)system(cmd);
    mkdir_p(SANDBOX_ROOT "/etc/vim", 0755);
    snprintf(cmd, sizeof(cmd), "cp -rL /etc/vim/* %s/etc/vim/ 2>/dev/null || true", SANDBOX_ROOT);
    (void)system(cmd);
    
    if (shell_copied) {
        log_action("Essential libraries, utilities, and terminfo copied to sandbox");
    } else {
        log_action("WARNING: No shell binary found to copy. Please install busybox or bash.");
    }
}

static void bind_host_tools(void) {
    struct stat st;
    char cmd[MAX_CMD];
    
    // ===== CRITICAL: Bind /sys FIRST for CPU info =====
    // This MUST happen before other mounts to avoid "Error reading the CPU table"
    mkdir_p(SANDBOX_ROOT "/sys", 0755);
    if (system("mount --rbind /sys " SANDBOX_ROOT "/sys") != 0) {
        log_action("Warning: Failed to bind /sys");
    }
    
    // Bind core directories
    const char *dirs[] = {"/bin", "/usr/bin", "/usr/sbin", "/lib", "/lib64", "/usr/lib", "/usr/libexec", "/usr/lib/sudo", "/usr/libexec/sudo", NULL};
    char target[PATH_MAX];
    for (int i = 0; dirs[i]; ++i) {
        if (stat(dirs[i], &st) == 0) {
            snprintf(target, sizeof(target), SANDBOX_ROOT "%s", dirs[i]);
            mkdir_p(target, 0755);
            snprintf(cmd, sizeof(cmd), "mount --bind %s %s", dirs[i], target);
            (void)system(cmd);
        }
    }
    
    // bind resolv.conf if present
    if (stat("/etc/resolv.conf", &st) == 0) {
        mkdir(SANDBOX_ROOT "/etc", 0755);
        ensure_file(SANDBOX_ROOT "/etc/resolv.conf");
        snprintf(cmd, sizeof(cmd), "mount --bind /etc/resolv.conf %s", SANDBOX_ROOT "/etc/resolv.conf");
        (void)system(cmd);
    }
    
    // bind ld cache and configs
    if (stat("/etc/ld.so.cache", &st) == 0) {
        ensure_file(SANDBOX_ROOT "/etc/ld.so.cache");
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
    
    // bind hostname for sudo to resolve host
    ensure_file(SANDBOX_ROOT "/etc/hostname");
    (void)system("mount --bind /etc/hostname " SANDBOX_ROOT "/etc/hostname");
    ensure_file(SANDBOX_ROOT "/etc/hosts");
    (void)system("mount --bind /etc/hosts " SANDBOX_ROOT "/etc/hosts");
    
    // ===== DEVICE NODES FOR APT/DPKG =====
    // Create /dev with proper permissions
    mkdir_p(SANDBOX_ROOT "/dev", 0755);
    
    // Essential device nodes (create before any bind mounts)
    (void)system("mknod -m 666 " SANDBOX_ROOT "/dev/null c 1 3 2>/dev/null || true");
    (void)system("mknod -m 666 " SANDBOX_ROOT "/dev/zero c 1 5 2>/dev/null || true");
    (void)system("mknod -m 666 " SANDBOX_ROOT "/dev/random c 1 8 2>/dev/null || true");
    (void)system("mknod -m 666 " SANDBOX_ROOT "/dev/urandom c 1 9 2>/dev/null || true");
    (void)system("mknod -m 666 " SANDBOX_ROOT "/dev/tty c 5 0 2>/dev/null || true");
    (void)system("mknod -m 666 " SANDBOX_ROOT "/dev/full c 1 7 2>/dev/null || true");
    
    // Setup PTY for sudo - CRITICAL for "unable to allocate pty" error
    mkdir_p(SANDBOX_ROOT "/dev/pts", 0755);
    (void)system("mount -t devpts devpts " SANDBOX_ROOT "/dev/pts -o gid=5,mode=620,ptmxmode=666 2>/dev/null || true");
    // Create ptmx device
    (void)system("rm -f " SANDBOX_ROOT "/dev/ptmx 2>/dev/null");
    (void)system("mknod -m 666 " SANDBOX_ROOT "/dev/ptmx c 5 2 2>/dev/null || true");
    // Alternative: link ptmx to pts/ptmx
    (void)system("ln -sf pts/ptmx " SANDBOX_ROOT "/dev/ptmx 2>/dev/null || true");
    
    // bind terminfo for clear, reset, etc. to work
    mkdir_p(SANDBOX_ROOT "/usr/share/terminfo", 0755);
    (void)system("mount --bind /usr/share/terminfo " SANDBOX_ROOT "/usr/share/terminfo");
    mkdir_p(SANDBOX_ROOT "/lib/terminfo", 0755);
    (void)system("mount --bind /lib/terminfo " SANDBOX_ROOT "/lib/terminfo 2>/dev/null || true");
    
    // ===== APT PACKAGE MANAGER SUPPORT =====
    // Bind apt configuration
    mkdir_p(SANDBOX_ROOT "/etc/apt", 0755);
    (void)system("mount --bind /etc/apt " SANDBOX_ROOT "/etc/apt");
    
    // Bind apt cache and state directories
    mkdir_p(SANDBOX_ROOT "/var/lib/apt", 0755);
    mkdir_p(SANDBOX_ROOT "/var/lib/apt/lists", 0755);
    mkdir_p(SANDBOX_ROOT "/var/lib/apt/lists/partial", 0755);
    (void)system("mount --bind /var/lib/apt " SANDBOX_ROOT "/var/lib/apt");
    
    mkdir_p(SANDBOX_ROOT "/var/cache/apt", 0755);
    mkdir_p(SANDBOX_ROOT "/var/cache/apt/archives", 0755);
    mkdir_p(SANDBOX_ROOT "/var/cache/apt/archives/partial", 0755);
    (void)system("mount --bind /var/cache/apt " SANDBOX_ROOT "/var/cache/apt");
    
    // ===== CRITICAL: DPKG DATABASE =====
    // Bind dpkg database with all subdirectories
    mkdir_p(SANDBOX_ROOT "/var/lib/dpkg", 0755);
    mkdir_p(SANDBOX_ROOT "/var/lib/dpkg/info", 0755);
    mkdir_p(SANDBOX_ROOT "/var/lib/dpkg/triggers", 0755);
    mkdir_p(SANDBOX_ROOT "/var/lib/dpkg/updates", 0755);
    (void)system("mount --bind /var/lib/dpkg " SANDBOX_ROOT "/var/lib/dpkg");
    
    // ===== CRITICAL: DEBCONF for package configuration =====
    mkdir_p(SANDBOX_ROOT "/var/cache/debconf", 0755);
    if (stat("/var/cache/debconf", &st) == 0) {
        (void)system("mount --bind /var/cache/debconf " SANDBOX_ROOT "/var/cache/debconf");
    }
    
    // Bind /usr/share/debconf for debconf templates
    mkdir_p(SANDBOX_ROOT "/usr/share/debconf", 0755);
    if (stat("/usr/share/debconf", &st) == 0) {
        (void)system("mount --bind /usr/share/debconf " SANDBOX_ROOT "/usr/share/debconf");
    }
    
    // Bind /usr/share/dpkg for dpkg scripts
    mkdir_p(SANDBOX_ROOT "/usr/share/dpkg", 0755);
    if (stat("/usr/share/dpkg", &st) == 0) {
        (void)system("mount --bind /usr/share/dpkg " SANDBOX_ROOT "/usr/share/dpkg");
    }
    
    // Bind apt logs
    mkdir_p(SANDBOX_ROOT "/var/log/apt", 0755);
    (void)system("mount --bind /var/log/apt " SANDBOX_ROOT "/var/log/apt 2>/dev/null || true");
    
    // Bind /var/log for dpkg logs
    mkdir_p(SANDBOX_ROOT "/var/log", 0755);
    ensure_file(SANDBOX_ROOT "/var/log/dpkg.log");
    (void)system("mount --bind /var/log/dpkg.log " SANDBOX_ROOT "/var/log/dpkg.log 2>/dev/null || true");
    
    // Bind /sbin for system utilities
    mkdir_p(SANDBOX_ROOT "/sbin", 0755);
    (void)system("mount --bind /sbin " SANDBOX_ROOT "/sbin");
    
    // ===== VIM CONFIGURATION =====
    mkdir_p(SANDBOX_ROOT "/usr/share/vim", 0755);
    (void)system("mount --bind /usr/share/vim " SANDBOX_ROOT "/usr/share/vim 2>/dev/null || true");
    mkdir_p(SANDBOX_ROOT "/etc/vim", 0755);
    (void)system("mount --bind /etc/vim " SANDBOX_ROOT "/etc/vim 2>/dev/null || true");
    
    // Bind alternatives for editor command
    mkdir_p(SANDBOX_ROOT "/etc/alternatives", 0755);
    (void)system("mount --bind /etc/alternatives " SANDBOX_ROOT "/etc/alternatives 2>/dev/null || true");
    
    // Bind locale
    mkdir_p(SANDBOX_ROOT "/usr/share/locale", 0755);
    (void)system("mount --bind /usr/share/locale " SANDBOX_ROOT "/usr/share/locale 2>/dev/null || true");
    
    // Bind perl lib for dpkg scripts
    mkdir_p(SANDBOX_ROOT "/usr/share/perl", 0755);
    if (stat("/usr/share/perl", &st) == 0) {
        (void)system("mount --bind /usr/share/perl " SANDBOX_ROOT "/usr/share/perl 2>/dev/null || true");
    }
    mkdir_p(SANDBOX_ROOT "/usr/share/perl5", 0755);
    if (stat("/usr/share/perl5", &st) == 0) {
        (void)system("mount --bind /usr/share/perl5 " SANDBOX_ROOT "/usr/share/perl5 2>/dev/null || true");
    }
    
    // Bind /run for various system utilities (lock files, etc.)
    mkdir_p(SANDBOX_ROOT "/run", 0755);
    mkdir_p(SANDBOX_ROOT "/run/lock", 0755);
    (void)system("mount --bind /run " SANDBOX_ROOT "/run 2>/dev/null || true");
    
    // Bind /tmp for apt/dpkg temp files - make it writable
    mkdir_p(SANDBOX_ROOT "/tmp", 01777);
    (void)system("chmod 1777 " SANDBOX_ROOT "/tmp");
    
    // Set environment file for DEBIAN_FRONTEND
    FILE *env_file = fopen(SANDBOX_ROOT "/etc/environment", "w");
    if (env_file) {
        fprintf(env_file, "DEBIAN_FRONTEND=noninteractive\n");
        fprintf(env_file, "DEBCONF_NONINTERACTIVE_SEEN=true\n");
        fprintf(env_file, "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin\n");
        fclose(env_file);
    }
    
    log_action("Network sandbox fully configured with enhanced apt support");
}

// Pipe file descriptor passed to child for synchronization
static int sync_pipe_fd = -1;

int setup_sandbox(void *arg) {
    struct SandboxConfig *config = (struct SandboxConfig *)arg;
    int should_run_shell = config ? 1 : 0; // Always run shell when config is provided
    
    // Wait for parent to set up uid/gid mappings before proceeding
    if (sync_pipe_fd >= 0) {
        char buf;
        if (read(sync_pipe_fd, &buf, 1) != 1) {
            perror("sync read");
            return 1;
        }
        close(sync_pipe_fd);
        sync_pipe_fd = -1;
    }
    
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
    mkdir("sys", 0755);
    mkdir("tmp", 0755);
    mkdir("dev", 0755);
    mkdir("etc", 0755);
    mkdir("run", 0755);

    // Mount proc
    if (mount("proc", "/proc", "proc", 0, NULL) == -1) {
        perror("mount proc");
        return 1;
    }
    
    // Mount sysfs for CPU info (needed by dpkg/apt)
    // IMPORTANT: Only mount sysfs for non-network sandboxes
    // Network sandboxes have /sys bind-mounted from host before chroot
    // Mounting sysfs on top would mask the bind mount with incomplete namespace-local view
    if (config && !config->network) {
        // Non-network mode: mount a namespace-local sysfs
        if (mount("sysfs", "/sys", "sysfs", 0, NULL) == -1) {
            fprintf(stderr, "Warning: Could not mount /sys filesystem. Some tools (apt, dpkg) may report errors.\\n");
        }
    } else {
        // Network mode: /sys should already be bind-mounted from host
        // Verify it's accessible
        struct stat sys_stat;
        if (stat("/sys/devices", &sys_stat) != 0) {
            fprintf(stderr, "Warning: /sys/devices not accessible. Package managers may fail.\\n");
        }
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
    
    // PTY devices - REQUIRED for sudo and proper terminal
    mkdir("/dev/pts", 0755);
    if (mount("devpts", "/dev/pts", "devpts", 0, "gid=5,mode=620,ptmxmode=666") == -1) {
        // Try without options if first attempt fails
        mount("devpts", "/dev/pts", "devpts", 0, NULL);
    }
    mknod("/dev/ptmx", S_IFCHR | 0666, makedev(5, 2));
    // Also create console device
    mknod("/dev/console", S_IFCHR | 0600, makedev(5, 1));
    // Create stdin/stdout/stderr symlinks
    symlink("/proc/self/fd", "/dev/fd");
    symlink("/proc/self/fd/0", "/dev/stdin");
    symlink("/proc/self/fd/1", "/dev/stdout");
    symlink("/proc/self/fd/2", "/dev/stderr");

    // Apply resource limits using our new functions
    if (config) {
        // Apply CPU cores limit using sched_setaffinity
        if (config->cpu_cores > 0) {
            apply_cpu_limit(config->cpu_cores);
        }
        
        // Apply memory limit using cgroups or rlimit
        if (config->memory > 0) {
            apply_memory_limit(config->memory);
        }
    }

    if (should_run_shell) {
        // Ensure essential config files inside chroot
        mkdir("/etc", 0755);
        
        // DNS resolver
        FILE *rf = fopen("/etc/resolv.conf", "w");
        if (rf) {
            fputs("nameserver 8.8.8.8\nnameserver 8.8.4.4\n", rf);
            fclose(rf);
        }
        
        // Hostname - needed for sudo
        char hostname[256] = "sandbox";
        gethostname(hostname, sizeof(hostname));
        FILE *hf = fopen("/etc/hostname", "w");
        if (hf) {
            fprintf(hf, "%s\n", hostname);
            fclose(hf);
        }
        
        // Hosts file - needed for hostname resolution
        FILE *hosts = fopen("/etc/hosts", "w");
        if (hosts) {
            fprintf(hosts, "127.0.0.1 localhost\n");
            fprintf(hosts, "127.0.0.1 %s\n", hostname);
            fprintf(hosts, "::1 localhost ip6-localhost ip6-loopback\n");
            fclose(hosts);
        }
        
        // Set environment variables for terminal and paths
        setenv("TERM", "xterm", 0);  // Don't override if already set
        setenv("TERMINFO", "/usr/share/terminfo", 1);
        setenv("PATH", "/bin:/usr/bin:/sbin:/usr/sbin", 1);
        setenv("HOME", "/", 1);
        setenv("USER", "root", 1);
        setenv("SHELL", "/bin/sh", 1);
        
        // Try multiple shells in order of preference
        const char *shells[] = {
            "/bin/busybox",
            "/bin/bash", 
            "/bin/sh",
            "/bin/dash",
            "/bin/zsh",
            "/usr/bin/bash",
            "/usr/bin/sh",
            NULL
        };
        
        struct stat st;
        for (int i = 0; shells[i] != NULL; i++) {
            if (stat(shells[i], &st) == 0 && (st.st_mode & S_IXUSR)) {
                // Shell exists and is executable
                if (strstr(shells[i], "busybox")) {
                    execl(shells[i], "busybox", "sh", NULL);
                } else {
                    execl(shells[i], "sh", NULL);
                }
                // If execl returns, there was an error
                perror(shells[i]);
            }
        }
        
        // If we get here, no shell was found
        fprintf(stderr, "Error: No shell found in sandbox. Tried: busybox, bash, sh\n");
        fprintf(stderr, "Make sure busybox or a shell is installed on the host system.\n");
        return 1;
    }
    return 0;
}

static void setup_uid_gid_map(pid_t pid, int use_user_ns) {
    if (!use_user_ns) return;
    
    char path[256];
    FILE *f;
    
    // Must deny setgroups before writing gid_map on modern kernels
    snprintf(path, sizeof(path), "/proc/%d/setgroups", pid);
    f = fopen(path, "w");
    if (f) {
        fprintf(f, "deny\n");
        fclose(f);
    }
    
    snprintf(path, sizeof(path), "/proc/%d/uid_map", pid);
    f = fopen(path, "w");
    if (f) {
        fprintf(f, "0 %d 1\n", getuid());
        fclose(f);
    }
    
    snprintf(path, sizeof(path), "/proc/%d/gid_map", pid);
    f = fopen(path, "w");
    if (f) {
        fprintf(f, "0 %d 1\n", getgid());
        fclose(f);
    }
}

int create_sandbox(int memory, int cpu_cores, int network, char *name) {
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
    snprintf(cmd, sizeof(cmd), "cp /bin/busybox %s/bin/ 2>/dev/null || true", SANDBOX_ROOT);
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
    } else {
        // For non-network sandboxes, still provide essential libraries
        bind_essential_libs();
    }

    /*
     * LINUX NAMESPACES USED:
     * - CLONE_NEWPID: PID namespace - processes in sandbox have separate PIDs
     * - CLONE_NEWNS: Mount namespace - filesystem mounts are isolated
     * - CLONE_NEWUTS: UTS namespace - hostname is isolated
     * For non-network (isolated) sandboxes, additionally:
     * - CLONE_NEWUSER: User namespace - UID/GID mapping, allows root in sandbox
     * - CLONE_NEWNET: Network namespace - completely isolated network stack
     */
    int flags = CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWUTS | SIGCHLD;
    int use_user_ns = 0;
    if (!network) {
        flags |= CLONE_NEWUSER | CLONE_NEWNET; // Full isolation when no network
        use_user_ns = 1;
    }

    // Create synchronization pipe
    int pipefd[2];
    if (pipe(pipefd) == -1) {
        perror("pipe");
        return 1;
    }

    struct SandboxConfig config = {memory, cpu_cores, network};
    sync_pipe_fd = pipefd[0]; // Child will read from this
    
    pid_t pid = clone(setup_sandbox, child_stack + STACK_SIZE, flags, &config);
    if (pid == -1) {
        perror("clone");
        close(pipefd[0]);
        close(pipefd[1]);
        return 1;
    }
    
    close(pipefd[0]); // Parent closes read end

    // Map uid/gid for user namespace (before signaling child)
    setup_uid_gid_map(pid, use_user_ns);
    
    // Signal child to proceed
    if (write(pipefd[1], "x", 1) != 1) {
        perror("sync write");
    }
    close(pipefd[1]);

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
            fprintf(config_file, "%s %d %d %d %ld\n", name, memory, cpu_cores, network, now);
            fclose(config_file);
        }
    }
    return rc;
}

int enter_sandbox(char *name) {
    log_action("Entering sandbox");

    struct SandboxConfig config = {100, 0, 0}; // default: 100MB, no CPU limit, no network
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
                    config.cpu_cores = c;
                    config.network = net;
                    break;
                }
            }
            fclose(f);
        }
    }

    // Ensure sandbox root directory exists and tmpfs is mounted
    struct stat st;
    if (stat(SANDBOX_ROOT, &st) == -1) {
        mkdir(SANDBOX_ROOT, 0755);
    }
    
    // Check if tmpfs is already mounted, if not mount it
    if (mount("tmpfs", SANDBOX_ROOT, "tmpfs", 0, NULL) == -1) {
        if (errno != EBUSY) { // EBUSY means already mounted, which is OK
            perror("mount tmpfs for enter");
            // Continue anyway, might work if already mounted
        }
    }
    
    // Create initial dirs if needed
    mkdir_p(SANDBOX_ROOT "/bin", 0755);
    mkdir_p(SANDBOX_ROOT "/usr/bin", 0755);
    mkdir_p(SANDBOX_ROOT "/lib", 0755);
    mkdir_p(SANDBOX_ROOT "/lib64", 0755);
    
    // Ensure busybox is available
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "cp /bin/busybox %s/bin/ 2>/dev/null || true", SANDBOX_ROOT);
    (void)system(cmd);

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
    } else {
        // For non-network sandboxes, still provide essential libraries
        bind_essential_libs();
    }

    // Use same namespaces as create_sandbox
    int flags = CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWUTS | SIGCHLD;
    int use_user_ns = 0;
    if (!config.network) {
        flags |= CLONE_NEWUSER | CLONE_NEWNET;
        use_user_ns = 1;
    }

    // Create synchronization pipe
    int pipefd[2];
    if (pipe(pipefd) == -1) {
        perror("pipe");
        return 1;
    }

    sync_pipe_fd = pipefd[0]; // Child will read from this
    
    pid_t pid = clone(setup_sandbox, child_stack + STACK_SIZE, flags, &config);
    if (pid == -1) {
        perror("clone");
        close(pipefd[0]);
        close(pipefd[1]);
        return 1;
    }
    
    close(pipefd[0]); // Parent closes read end

    // Map uid/gid for user namespace (before signaling child)
    setup_uid_gid_map(pid, use_user_ns);
    
    // Signal child to proceed
    if (write(pipefd[1], "x", 1) != 1) {
        perror("sync write");
    }
    close(pipefd[1]);

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
    snprintf(cmd, sizeof(cmd), "umount %s 2>/dev/null || true", SANDBOX_ROOT);
    (void)system(cmd);
    rmdir(SANDBOX_ROOT);
    return 0;
}

int main(int argc, char *argv[]) {
    int memory = 1024; // MB - default 1GB
    int cpu_cores = 0; // 0 = no limit (use all cores)
    int network = 0; // 0 disable, 1 enable
    int create = 0, enter = 0, delete = 0;
    char *name = NULL;
    
    int opt;
    while ((opt = getopt(argc, argv, "cedm:p:ns:")) != -1) {
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
            case 'p':
                cpu_cores = atoi(optarg);
                break;
            case 'n':
                network = 1;
                break;
            case 's':
                name = optarg;
                break;
            default:
                fprintf(stderr, "Usage: %s -c (create) -e (enter) -d (delete) [-m memory(MB)] [-p cpu_cores] [-n (enable network)] [-s name]\n", argv[0]);
                return 1;
        }
    }
    
    // Validate mutually exclusive options
    int action_count = create + enter + delete;
    if (action_count == 0) {
        fprintf(stderr, "Error: Must specify one of -c, -e, or -d\n");
        fprintf(stderr, "Usage: %s -c (create) -e (enter) -d (delete) [-m memory(MB)] [-p cpu_cores] [-n (enable network)] [-s name]\n", argv[0]);
        return 1;
    }
    
    if (action_count > 1) {
        fprintf(stderr, "Error: Cannot specify more than one of -c, -e, or -d\n");
        return 1;
    }
    
    int rc = 0;
    
    // Check system requirements before proceeding
    if (!check_system_requirements()) {
        fprintf(stderr, "System requirements not met. See warnings above.\n");
        return 1;
    }
    
    if (create) {
        rc = create_sandbox(memory, cpu_cores, network, name);
    } else if (enter) {
        rc = enter_sandbox(name);
    } else if (delete) {
        rc = delete_sandbox();
    }
    
    return rc;
}