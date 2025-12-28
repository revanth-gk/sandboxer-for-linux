# 🔒 Linux Sandbox Manager

A powerful Linux sandbox program written in C that creates isolated environments using Linux namespaces. Features both a command-line interface and a modern GTK-based GUI.

![License](https://img.shields.io/badge/license-MIT-blue.svg)
![Platform](https://img.shields.io/badge/platform-Linux-green.svg)
![Language](https://img.shields.io/badge/language-C-orange.svg)

---

## ✨ Features

### Core Sandbox Technology
- **Linux Namespaces**: Full isolation using PID, Mount, UTS, User, and Network namespaces
- **Chroot Environment**: Secure root filesystem isolation
- **Tmpfs Root**: Ephemeral storage - data is wiped on sandbox exit
- **Resource Limits**: Configurable memory and CPU time limits
- **Two Modes**: Isolated (no network) and Connected (with network) modes

### User Interface
- **GTK3 GUI**: Modern graphical interface with system monitoring
- **CLI Tool**: Full command-line interface for scripting
- **Real-time Monitoring**: CPU and memory usage display
- **Log Management**: Built-in logging with export capability

---

## 📊 Sandbox Modes Comparison

### 🔒 Non-Network Sandbox (Maximum Isolation)

| Feature | Status | Details |
|---------|--------|---------|
| **Security Level** | ⭐⭐⭐⭐⭐ | Maximum isolation |
| **Network Access** | ❌ None | Completely isolated network stack |
| **Root Required** | ❌ No | Works without sudo |
| **Package Manager** | ❌ No | Cannot install packages |
| **File Persistence** | ❌ No | Data lost on exit |

**Linux Namespaces Used:**
| Namespace | Purpose |
|-----------|---------|
| PID | Process isolation - sandbox has PID 1 |
| Mount | Isolated filesystem mounts |
| UTS | Isolated hostname |
| User | UID/GID mapping - root inside = unprivileged outside |
| Network | Completely isolated network stack |

**Available Commands:**
- **File Operations**: `ls`, `cat`, `cp`, `mv`, `rm`, `touch`, `chmod`, `mkdir`, `pwd`
- **Text Editors**: `vi`, `vim`, `nano` (if installed on host)
- **Text Processing**: `grep`, `sed`, `head`, `tail`, `wc`, `sort`, `cut`, `awk`
- **Terminal**: `clear`, `reset`, `tput`
- **Utilities**: `ps`, `kill`, `find`, `xargs`, `env`, `id`, `whoami`

**Best For:**
- 🛡️ Running untrusted scripts
- 🧪 Testing in isolated environment
- 🔐 Security-sensitive operations

---

### 🌐 Network Sandbox (Development Environment)

| Feature | Status | Details |
|---------|--------|---------|
| **Security Level** | ⭐⭐⭐ | Partial isolation |
| **Network Access** | ✅ Full | Internet access via NAT |
| **Root Required** | ✅ Yes | Needed for network setup |
| **Package Manager** | ✅ Yes | apt update/install works |
| **File Persistence** | ❌ No | Data lost on exit |

**Linux Namespaces Used:**
| Namespace | Purpose |
|-----------|---------|
| PID | Process isolation |
| Mount | Chroot with host bind mounts |
| UTS | Isolated hostname |

**Full Host Access:**
- All commands from `/bin`, `/usr/bin`, `/sbin`
- All libraries from `/lib`, `/lib64`, `/usr/lib`
- Package management with `apt`
- Network tools: `wget`, `curl`, `git`, `ssh`

**Best For:**
- 💻 Development and testing
- 📦 Installing and testing packages
- 🔧 Building and compiling projects

---

## 📈 Quick Feature Matrix

| Feature | No Network | With Network |
|---------|:----------:|:------------:|
| Security Level | ⭐⭐⭐⭐⭐ | ⭐⭐⭐ |
| Functionality | ⭐⭐ | ⭐⭐⭐⭐⭐ |
| Requires sudo | ❌ | ✅ |
| Internet Access | ❌ | ✅ |
| apt install | ❌ | ✅ |
| All host commands | ❌ | ✅ |
| File editors | ✅ | ✅ |
| Process isolation | ✅ | ✅ |
| Filesystem isolation | ✅ | ✅ |

---

## 🛠️ Build Instructions

### Prerequisites

```bash
# Ubuntu/Debian
sudo apt update
sudo apt install build-essential libgtk-3-dev libvte-2.91-dev busybox-static

# Fedora
sudo dnf install gcc make gtk3-devel vte291-devel busybox
```

### Compile

```bash
git clone <repository-url>
cd sandboxer-for-linux
make all
```

### Output
- `bin/sandbox` - Command-line tool
- `bin/gui` - Graphical interface

---

## 🚀 Usage

### GUI Mode

```bash
# For network-enabled sandboxes (requires root)
sudo ./bin/gui

# For isolated sandboxes only
./bin/gui
```

### CLI Mode

```bash
# Create a sandbox
./bin/sandbox -c -s mysandbox -m 256 -t 60

# Create with network (requires root)
sudo ./bin/sandbox -c -s mysandbox -m 256 -t 60 -n

# Enter a sandbox
./bin/sandbox -e -s mysandbox

# Delete sandbox
./bin/sandbox -d
```

### CLI Options

| Option | Description | Default |
|--------|-------------|---------|
| `-c` | Create sandbox | - |
| `-e` | Enter sandbox | - |
| `-d` | Delete sandbox | - |
| `-s <name>` | Sandbox name | - |
| `-m <MB>` | Memory limit in MB | 100 |
| `-t <sec>` | CPU time limit in seconds | 10 |
| `-n` | Enable network access | disabled |

---

## 📁 Project Structure

```
sandboxer-for-linux/
├── src/
│   ├── main.c          # Core sandbox logic
│   └── gui.c           # GTK GUI implementation
├── bin/
│   ├── sandbox         # CLI executable
│   └── gui             # GUI executable
├── build/              # Object files
├── Makefile
├── README.md
├── sandboxes.txt       # Sandbox configurations
└── gui.log             # GUI activity log
```

---

## ⚙️ System Requirements

| Requirement | Details |
|-------------|---------|
| **Operating System** | Linux (kernel 3.8+) |
| **Architecture** | x86_64 (AMD64) |
| **Namespace Support** | User namespaces enabled |
| **Dependencies** | GTK+ 3.0, VTE 2.91, busybox |

### Check Namespace Support

```bash
# Check if user namespaces are enabled
cat /proc/sys/kernel/unprivileged_userns_clone
# Should output: 1

# Enable if disabled
sudo sysctl -w kernel.unprivileged_userns_clone=1
```

---

## 🔧 Troubleshooting

### Common Issues

| Issue | Solution |
|-------|----------|
| "clone failed: Operation not permitted" | Enable user namespaces or run as root |
| "No shell found" | Install busybox: `sudo apt install busybox-static` |
| "Error reading CPU table" | Non-fatal warning, packages still install |
| "unable to resolve host" | Fixed in latest version |
| "unable to allocate pty" | Fixed in latest version |

### WSL2 Notes

WSL2 is supported with some limitations:
- User namespaces should work
- Network namespaces may have restrictions
- Run with `sudo` for full functionality

---

## 📜 License

This project is open source. See LICENSE file for details.

---

## 🤝 Contributing

Contributions are welcome! Please feel free to submit issues and pull requests.

---

## 📞 Support

If you encounter any issues, please open a GitHub issue with:
- Your Linux distribution and kernel version
- The exact error message
- Steps to reproduce the problem
