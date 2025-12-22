# Linux Sandbox Program

A minimal yet functional Linux sandbox program written in C that creates isolated environments using Linux namespaces.

## Features

- **Namespaces**: Uses PID, user, network, and mount namespaces for isolation.
- **Chroot**: Changes root to a separate filesystem.
- **Tmpfs Root**: Mounts tmpfs as the sandbox's root directory.
- **Filesystem Structure**: Creates essential directories (/bin, /proc, /tmp, /dev, /etc).
- **Shell**: Runs busybox shell inside the sandbox.
- **Resource Limits**: Sets memory (100MB) and CPU (10s) limits using ulimit.
- **Logging**: Logs actions to /tmp/sandbox.log.
- **CLI**: Command-line interface for create, enter, delete sandboxes.

## Build

```bash
make
```

## Usage

Run as root (requires privileges for namespaces).

- Create sandbox: `sudo ./bin/sandbox -c`
- Enter sandbox: `sudo ./bin/sandbox -e`
- Delete sandbox: `sudo ./bin/sandbox -d`

Inside the sandbox, you can perform file operations like creating/deleting files using shell commands.

## Requirements

- Linux with namespace support
- busybox installed
- Root privileges

## Limitations

- Basic implementation, no advanced cgroups for resource control.
- File operations via shell, not direct C functions in CLI.
