# What's new

Back to the [README](../README.md). Each version's details are in [Features](features.md).

## 0.6.0

**Logging on**
- **SSH certificates:** a key's OpenSSH certificate (`key-cert.pub`) is sent with it, or a certificate chosen per session. The session window shows who it is for and until when, in red when it has expired or is for another user.
- **FIDO2 security keys** (`sk-ed25519`, `sk-ecdsa`; YubiKey, Windows Hello): logging on with a touch and, when the key needs it, its PIN. **Tools → SSH Keys** creates them (optionally kept on the key) and copies the keys kept on a security key.
- **Kerberos (GSSAPI)** on Linux: logging on with your domain ticket, optionally passing it on (`ssh -K`), for everything, a folder, or a session; Preferences show the ticket and run `kinit`.

**Automation**
- **Login scripts and triggers** (a session's or folder's **Automation** tab): wait for a text, then type an answer; watch the output and type, notify, beep, or mark the tab.
- **Saved scripts** (**Tools → Scripts**, kept in `settings/scripts/`): run whole on a server, in a tab, on many hosts, after logging on, or inside a container; with an interpreter, optional `sudo`, and `{fields}` asked for when they run.
- **Commands** and **Scripts** tabs beside Sessions and Files, with search and groups; **View** shows or hides each tab.

**New session types**
- **Local**: a shell of this computer (PowerShell, PowerShell 7, Command Prompt, Git Bash; bash, zsh, fish…) or any program, with **Run as administrator** (Windows UAC, Windows sudo; Linux sudo or pkexec).
- **WSL** (Windows): a chosen distribution, user and folder.
- **Container (Docker)**: a shell in a container, here, through a Docker context, on a remote engine over TCP with TLS, or on an SSH server; Podman too. **Open a Container…** on SSH sessions.
- **Container (Kubernetes)**: a shell in a pod, with the default kubeconfig, a saved credential, a kubeconfig file, a token, or a client certificate. **Tools → Kubernetes Credentials** keeps kubeconfigs encrypted or as private files.
- Their lists (distributions, containers, contexts, namespaces, pods) fill themselves, with their state on a line under each.

**Preferences**
- New categories: **Connections** has one tab per session type; **Network** (proxy, files and transfers), **Security and vault** (now with keys and certificates, Kerberos) and **Tools** (Run on hosts, HTTP requests, scripts).
- Every main feature can be turned off.
- Each session window opens on its protocol's own tab.

**Under the hood**
- libssh 0.12.2 in both packages (built from its signed release), with libfido2 and libcbor on Windows and MIT Kerberos on Linux.
