# SSH Server Support

## Overview

MudOS-NG now supports SSH protocol connections via libssh. SSH connections are functionally identical to telnet — same LPC `master::connect()` / `user::logon()` pipeline for IP banning and user authentication.

## Quick Start

### Install libssh

```bash
# macOS
brew install libssh

# Ubuntu/Debian
sudo apt install libssh-dev

# Fedora/RHEL
sudo dnf install libssh-devel
```

### Configure

Add to your mudos config file:

```ini
ssh port : 3999
```

If not specified, defaults to `port - 1` (where `port` is the legacy `port number` option). Set to `0` to disable SSH.

### Connect

```bash
ssh -p 3999 user@your-mud-server
```

Any username/password is accepted at the SSH protocol layer. Actual authentication and banning is handled by LPC `master::connect()` and `user::logon()` — identical to telnet.

Host keys are auto-generated on first run and stored in `{config_dir}/.ssh-config`.

## Architecture

### Connection Flow

```
TCP accept → new_conn_handler(PORT_TYPE_SSH)
  → new_user(port, fd, addr)             [allocates interactive_t]
  → io_thread->post:
      → ssh_conn_accept(bind, fd, base, user)
        → ssh_bind_accept_fd()             [attach fd to session]
        → ssh_set_blocking(session, 0)     [non-blocking mode]
        → event_new(fd, EV_READ, ssh_event_cb, conn)  [libevent]

SSH handshake (ssh_event_cb, on IO thread):
  → ssh_handle_key_exchange()  → SSH_STATE_HANDSHAKE → SSH_STATE_AUTH
  → ssh_message_get()          → password auth        → SSH_STATE_CHANNEL
  → channel open               → SSH_STATE_READY
  → event_base_once(g_event_base, on_user_logon, user)

Logon (on VM thread):
  → master::connect(port)      [IP/user ban check]
  → user::logon()              [user authentication]

Normal I/O (on IO thread):
  Read:  ssh_event_cb → ssh_channel_read_nonblocking → on_user_input
  Write: add_message → output_to_user → ssh_conn_write → ssh_channel_write

Disconnect:
  ssh_channel_is_eof / SSH_ERROR → NET_DEAD → remove_interactive
```

### State Machine

| State | Description |
|-------|------------|
| `SSH_STATE_HANDSHAKE` | Key exchange in progress |
| `SSH_STATE_AUTH` | Waiting for password authentication |
| `SSH_STATE_CHANNEL` | Waiting for session channel open |
| `SSH_STATE_READY` | Normal I/O |
| `SSH_STATE_ERROR` | Fatal error, tear down |

### Host Key Management

On first run, the driver:
1. Generates an RSA 2048-bit key via `ssh_pki_generate_key(SSH_KEYTYPE_RSA)`
2. Exports to `{config_dir}/.ssh-config` in PEM format (chmod 600)
3. Reads the PEM file back as a string
4. Sets the key on the bind via `SSH_BIND_OPTIONS_IMPORT_KEY_STR` (the file-path-based `SSH_BIND_OPTIONS_IMPORT_KEY` is broken on macOS/libssh 0.12)

On subsequent runs, the PEM file is read directly and imported via `SSH_BIND_OPTIONS_IMPORT_KEY_STR`.

## Files

### New Files

| File | Purpose |
|------|---------|
| `src/net/ssh.h` | Public API: opaque types, state enum, function declarations |
| `src/net/ssh.cc` | ~370 lines: host key gen, libevent-driven state machine, I/O buffering |

### Modified Files

| File | Changes |
|------|---------|
| `src/base/internal/external_port.h` | `PORT_TYPE_SSH = 6` enum, `ssh_bind_struct *` + `ssh_config_dir` fields, array `[5]→[6]` |
| `src/base/internal/external_port.cc` | Array size `[6]` |
| `src/base/internal/rc.cc` | Parse `ssh port : <int>`, default to `port - 1`, store config dir |
| `src/interactive.h` | `ssh_conn *ssh_conn` field |
| `src/comm.cc` | SSH accept branch, output routing, cleanup, no-op read path |
| `src/CMakeLists.txt` | `net/ssh.cc` source, libssh detection (pkg-config + fallback), linking |

## Thread Safety

All SSH I/O runs on the IO thread that owns the connection. The VM thread never touches `ssh_conn` or `ssh_session`. Cross-thread dispatch uses the existing patterns:

- `ssh_event_cb` runs on IO thread → schedules `on_user_logon` on VM thread via `event_base_once(g_event_base, ...)`
- `add_message()` posts `output_to_user()` to IO thread via `ip->io_thread->post()`
- `remove_interactive()` posts `ssh_conn_free()` to IO thread if not already there

## Implementation Notes

### Macro Conflict

libssh's `legacy.h` declares `ssh_string string_copy(ssh_string str)`, which conflicts with mudos's `#define string_copy(x, y)` macro in `stralloc.h`. Resolved in `ssh.cc` by:
```cpp
#ifdef string_copy
#undef string_copy
#endif
#include <libssh/libssh.h>
#include <libssh/server.h>
// ...
#include "base/internal/stralloc.h"  // re-import the macro
```

### Opaque Types

`ssh.h` uses opaque forward declarations (`struct ssh_bind_struct *`) instead of including libssh headers. This prevents the macro conflict from spreading to other translation units.

### Write Buffering

If `ssh_channel_write()` returns `SSH_AGAIN`, data is buffered in `ssh_conn::wbuf` and a `EV_WRITE` event is registered on the fd. The write is retried on the next `EV_WRITE` callback.

### Password Auth

Any non-empty password is accepted at the SSH level. The SSH `SSH_REQUEST_AUTH` message with `SSH_AUTH_METHOD_PASSWORD` triggers `ssh_message_auth_reply_success(msg, 0)`. Non-password auth methods are rejected with `ssh_message_auth_set_methods(msg, SSH_AUTH_METHOD_PASSWORD)`.

### Line Ending Conversion

SSH sends raw bytes without protocol-level escaping. Bare `\n` characters cause the "staircase effect" (each line shifts right). The driver converts `\n` → `\r\n` in `ssh_conn_write()` (when not already preceded by `\r`), matching telnet's behavior.

### Channel Requests

SSH clients may send channel requests after the session opens:
- **PTY** (`SSH_CHANNEL_REQUEST_PTY`): Accepted. Use `ssh -t` to force PTY allocation.
- **Shell** (`SSH_CHANNEL_REQUEST_SHELL`): Accepted.
- **Window change**: Not yet forwarded to LPC.

### Input Processing

After reading SSH channel data, `cmd_in_buf()` is called to detect complete commands and schedule processing via the `ev_command` timer — same flow as telnet.

## Verification

1. **Build**: `cmake .. -DCMAKE_BUILD_TYPE=Release && make -j$(nproc) install`
2. **Smoke test**: `ssh -p <port> localhost`, enter any password, verify LPC `logon()` is called
3. **Ban test**: Reject IP in `master::connect()`, verify SSH connection dropped
4. **Key generation**: Delete `.ssh-config`, restart driver, verify auto-generation
5. **Concurrent test**: Telnet + SSH simultaneously

## Dependencies

- **libssh** ≥ 0.9.0 (system package)
- Requires `ssh_pki_generate_key()`, `ssh_bind_accept_fd()`, `ssh_channel_read_nonblocking()`
- OpenSSL for crypto (already required by MudOS-NG)

## Known Limitations (v1)

- Terminal dimensions from SSH PTY requests are not forwarded to LPC (`APPLY_WINDOW_SIZE`)
- Only password authentication is supported (no public key)
- No SSH-specific LPC efuns to query connection metadata
