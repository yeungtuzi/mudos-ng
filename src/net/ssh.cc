/*
 * Copyright (c) 2026 [大河马/dahema@me.com]
 * SPDX-License-Identifier: MIT
 */

#include "base/std.h"

// Undef mudos macros that conflict with libssh identifiers
#ifdef string_copy
#undef string_copy
#endif

#include <libssh/libssh.h>
#include <libssh/server.h>

#include "net/ssh.h"

#include <event2/event.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string>
#include <vector>

#include "comm.h"

// libssh 0.9.x compatibility
#if LIBSSH_VERSION_MAJOR == 0 && LIBSSH_VERSION_MINOR < 10
#define ssh_pki_generate_key ssh_pki_generate_key_compat
static inline int ssh_pki_generate_key_compat(int type, const char *, ssh_key *pkey) {
  return ssh_pki_generate(static_cast<ssh_keytypes_e>(type), 2048, pkey);
}
#endif
#include "interactive.h"

// Re-import mudos string macros (lost when we undef'd string_copy above)
#include "base/internal/stralloc.h"

// Global event base, declared in backend.cc
extern event_base *g_event_base;

// ---------------------------------------------------------------------------
// Per-connection SSH context
// ---------------------------------------------------------------------------
struct ssh_conn {
  ssh_session session{nullptr};
  ssh_channel channel{nullptr};
  event *ev{nullptr};
  event_base *base{nullptr};
  interactive_t *user{nullptr};
  ssh_conn_state state{SSH_STATE_HANDSHAKE};

  // Write buffering
  std::vector<char> wbuf;

  // Temporary read buffer
  char rbuf[8192];
};

// Forward declarations
static void ssh_event_cb(evutil_socket_t fd, short what, void *arg);

// ---------------------------------------------------------------------------
// Host key management
// ---------------------------------------------------------------------------

static int ssh_ensure_hostkey(const std::string &config_dir) {
  auto key_path = config_dir + "/.ssh-config";

  // Check if key file already exists and is non-empty
  struct stat st;
  if (stat(key_path.c_str(), &st) == 0 && st.st_size > 0) {
    return 0;  // Key exists
  }

  // Generate a new RSA 2048-bit key
  ssh_key key{nullptr};
  int rc = ssh_pki_generate_key(SSH_KEYTYPE_RSA, nullptr, &key);
  if (rc != SSH_OK) {
    debug_message("SSH: failed to generate host key.\n");
    return -1;
  }

  // Export to PEM file directly (passphrase=nullptr, no auth callback)
  rc = ssh_pki_export_privkey_file(key, nullptr, nullptr, nullptr, key_path.c_str());
  ssh_key_free(key);
  if (rc != SSH_OK) {
    debug_message("SSH: failed to write host key to %s.\n", key_path.c_str());
    return -1;
  }

  // Restrict permissions
  chmod(key_path.c_str(), 0600);

  debug_message("SSH: generated host key at %s.\n", key_path.c_str());
  return 0;
}

// ---------------------------------------------------------------------------
// Server lifecycle
// ---------------------------------------------------------------------------

ssh_bind ssh_server_init(const char *config_dir) {
  std::string dir(config_dir);
  auto key_path = dir + "/.ssh-config";

  // Load or generate the host key
  ssh_key hostkey{nullptr};
  struct stat st;
  if (stat(key_path.c_str(), &st) == 0 && st.st_size > 0) {
    // Key file exists — load it
    int rc = ssh_pki_import_privkey_file(key_path.c_str(), nullptr, nullptr, nullptr, &hostkey);
    if (rc != SSH_OK) {
      debug_message("SSH: failed to load host key from %s, regenerating.\n", key_path.c_str());
      // Fall through to regenerate
    }
  }

  if (!hostkey) {
    int rc = ssh_pki_generate_key(SSH_KEYTYPE_RSA, nullptr, &hostkey);
    if (rc != SSH_OK) {
      debug_message("SSH: failed to generate host key.\n");
      return nullptr;
    }
    ssh_pki_export_privkey_file(hostkey, nullptr, nullptr, nullptr, key_path.c_str());
    chmod(key_path.c_str(), 0600);
    debug_message("SSH: generated host key at %s.\n", key_path.c_str());
  }

  auto bind = ssh_bind_new();
  if (!bind) {
    ssh_key_free(hostkey);
    return nullptr;
  }
  ssh_key_free(hostkey);

  int rc;
#if LIBSSH_VERSION_MAJOR > 0 || LIBSSH_VERSION_MINOR >= 10
  // Read PEM file into string for SSH_BIND_OPTIONS_IMPORT_KEY_STR
  std::string pem;
  {
    int fd = open(key_path.c_str(), O_RDONLY);
    if (fd < 0) {
      debug_message("SSH: failed to open host key %s.\n", key_path.c_str());
      ssh_bind_free(bind);
      return nullptr;
    }
    char buf[4096];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
      pem.append(buf, n);
    }
    close(fd);
  }
  rc = ssh_bind_options_set(bind, SSH_BIND_OPTIONS_IMPORT_KEY_STR, pem.c_str());
#else
  rc = ssh_bind_options_set(bind, SSH_BIND_OPTIONS_IMPORT_KEY, key_path.c_str());
#endif
  if (rc != SSH_OK) {
    debug_message("SSH: failed to import host key (err=%d).\n", rc);
    ssh_bind_free(bind);
    return nullptr;
  }

  debug_message("SSH server initialized with host key %s.\n", key_path.c_str());
  return bind;
}

void ssh_server_close(ssh_bind bind) {
  if (bind) {
    ssh_bind_free(bind);
  }
}

// ---------------------------------------------------------------------------
// Per-connection lifecycle
// ---------------------------------------------------------------------------

ssh_conn *ssh_conn_accept(ssh_bind bind, evutil_socket_t fd, event_base *base,
                          interactive_t *user) {
  auto *conn = new ssh_conn;
  conn->base = base;
  conn->user = user;

  conn->session = ssh_new();
  if (!conn->session) {
    delete conn;
    return nullptr;
  }

  int rc = ssh_bind_accept_fd(bind, conn->session, fd);
  if (rc != SSH_OK) {
    debug_message("SSH: ssh_bind_accept_fd failed: %s.\n",
                  ssh_get_error(conn->session));
    ssh_free(conn->session);
    delete conn;
    return nullptr;
  }

  ssh_set_blocking(conn->session, 0);

  conn->ev = event_new(base, fd, EV_READ | EV_PERSIST, ssh_event_cb, conn);
  if (!conn->ev) {
    ssh_disconnect(conn->session);
    ssh_free(conn->session);
    delete conn;
    return nullptr;
  }
  event_add(conn->ev, nullptr);

  debug(connections, "SSH: new connection, starting handshake.\n");
  return conn;
}

void ssh_conn_free(ssh_conn *conn) {
  if (!conn) return;

  if (conn->ev) {
    event_del(conn->ev);
    event_free(conn->ev);
  }

  if (conn->channel) {
    ssh_channel_close(conn->channel);
    ssh_channel_free(conn->channel);
  }

  if (conn->session) {
    ssh_disconnect(conn->session);
    ssh_free(conn->session);
  }

  delete conn;
}

// ---------------------------------------------------------------------------
// State machine helpers
// ---------------------------------------------------------------------------

static void ssh_flush_writes(ssh_conn *conn) {
  if (conn->wbuf.empty()) return;

  int n = ssh_channel_write(conn->channel, conn->wbuf.data(), conn->wbuf.size());
  if (n > 0) {
    conn->wbuf.erase(conn->wbuf.begin(), conn->wbuf.begin() + n);
  }

  if (conn->wbuf.empty() && conn->ev) {
    // Remove EV_WRITE, keep EV_READ only
    event_del(conn->ev);
    event_assign(conn->ev, conn->base, event_get_fd(conn->ev),
                 EV_READ | EV_PERSIST, ssh_event_cb, conn);
    event_add(conn->ev, nullptr);
  }
}

static void ssh_do_reads(ssh_conn *conn) {
  interactive_t *ip = conn->user;
  int n;
  while ((n = ssh_channel_read_nonblocking(conn->channel,
            conn->rbuf, sizeof(conn->rbuf), 0)) > 0) {
    on_user_input(ip, conn->rbuf, n);
  }

  // Check for complete command (same as get_user_data does for telnet)
  if (cmd_in_buf(ip)) {
    ip->iflags |= CMD_IN_BUF;
    struct timeval zero_sec = {0, 0};
    evtimer_del(ip->ev_command);
    evtimer_add(ip->ev_command, &zero_sec);
  }

  if (ssh_channel_is_eof(conn->channel)) {
    debug(connections, "SSH: channel EOF.\n");
    ip->iflags |= NET_DEAD;
    remove_interactive(ip->ob, 0);
  }
}

// ---------------------------------------------------------------------------
// Main event callback — drives the SSH state machine
// ---------------------------------------------------------------------------

static void ssh_event_cb(evutil_socket_t /*fd*/, short what, void *arg) {
  auto *conn = static_cast<ssh_conn *>(arg);
  interactive_t *ip = conn->user;

  // Flush pending writes if we're in READY state
  if ((what & EV_WRITE) && conn->state == SSH_STATE_READY) {
    ssh_flush_writes(conn);
    return;
  }

  // --- SSH_STATE_HANDSHAKE ---
  if (conn->state == SSH_STATE_HANDSHAKE) {
    int rc = ssh_handle_key_exchange(conn->session);
    if (rc == SSH_OK) {
      conn->state = SSH_STATE_AUTH;
      debug(connections, "SSH: handshake complete, waiting for auth.\n");
    } else if (rc == SSH_ERROR) {
      debug_message("SSH: key exchange failed: %s.\n", ssh_get_error(conn->session));
      goto error;
    } else {
      return;  // SSH_AGAIN
    }
  }

  // --- SSH_STATE_AUTH ---
  if (conn->state == SSH_STATE_AUTH) {
    ssh_message msg = ssh_message_get(conn->session);
    if (msg) {
      if (ssh_message_type(msg) == SSH_REQUEST_AUTH &&
          ssh_message_subtype(msg) == SSH_AUTH_METHOD_PASSWORD) {
        ssh_message_auth_reply_success(msg, 0);
        ssh_message_free(msg);
        conn->state = SSH_STATE_CHANNEL;
        debug(connections, "SSH: password auth succeeded.\n");
      } else if (ssh_message_type(msg) == SSH_REQUEST_AUTH) {
        ssh_message_auth_set_methods(msg, SSH_AUTH_METHOD_PASSWORD);
        ssh_message_reply_default(msg);
        ssh_message_free(msg);
      } else {
        ssh_message_reply_default(msg);
        ssh_message_free(msg);
      }
    }
  }

  // --- SSH_STATE_CHANNEL ---
  if (conn->state == SSH_STATE_CHANNEL) {
    ssh_message msg = ssh_message_get(conn->session);
    if (msg) {
      if (ssh_message_type(msg) == SSH_REQUEST_CHANNEL_OPEN &&
          ssh_message_subtype(msg) == SSH_CHANNEL_SESSION) {
        conn->channel = ssh_message_channel_request_open_reply_accept(msg);
        ssh_message_free(msg);
        if (conn->channel) {
          conn->state = SSH_STATE_READY;
          debug(connections, "SSH: channel opened, scheduling logon.\n");

          event_base_once(g_event_base, -1, EV_TIMEOUT,
                          [](evutil_socket_t, short, void *uptr) {
                            auto *user = static_cast<interactive_t *>(uptr);
                            on_user_logon(user);
                            current_interactive = nullptr;
                          },
                          ip, nullptr);
        } else {
          debug_message("SSH: failed to accept channel open.\n");
          goto error;
        }
      } else {
        ssh_message_reply_default(msg);
        ssh_message_free(msg);
      }
    } else if (ssh_get_error_code(conn->session) == SSH_ERROR) {
      debug_message("SSH: error waiting for channel: %s.\n",
                    ssh_get_error(conn->session));
      goto error;
    }
  }

  // --- SSH_STATE_READY ---
  if (conn->state == SSH_STATE_READY) {
    // Handle channel requests (PTY, shell, window-change, etc.)
    ssh_message msg = ssh_message_get(conn->session);
    if (msg) {
      if (ssh_message_type(msg) == SSH_REQUEST_CHANNEL) {
        int subtype = ssh_message_subtype(msg);
        if (subtype == SSH_CHANNEL_REQUEST_PTY || subtype == SSH_CHANNEL_REQUEST_SHELL) {
          debug(connections, "SSH: channel request %d accepted.\n", subtype);
          ssh_message_channel_request_reply_success(msg);
        } else {
          ssh_message_reply_default(msg);
        }
      } else {
        ssh_message_reply_default(msg);
      }
      ssh_message_free(msg);
    }

    ssh_do_reads(conn);
  }

  return;

error:
  conn->state = SSH_STATE_ERROR;
  ip->iflags |= NET_DEAD;
  remove_interactive(ip->ob, 0);
}

// ---------------------------------------------------------------------------
// Public I/O API
// ---------------------------------------------------------------------------

int ssh_conn_write(ssh_conn *conn, const char *buf, size_t len) {
  if (!conn || conn->state != SSH_STATE_READY || !conn->channel) return -1;

  // Convert bare \n to \r\n (telnet does this automatically, SSH doesn't)
  const char *src = buf;
  size_t remaining = len;
  std::string converted;
  while (remaining > 0) {
    const char *nl = static_cast<const char *>(memchr(src, '\n', remaining));
    if (!nl) {
      converted.append(src, remaining);
      break;
    }
    // Only add \r if this \n is not already preceded by \r
    if (nl == src || nl[-1] != '\r') {
      converted.append(src, nl - src);
      converted.append("\r\n", 2);
    } else {
      converted.append(src, nl - src + 1);
    }
    src = nl + 1;
    remaining = len - (src - buf);
  }

  const char *out = converted.empty() ? buf : converted.data();
  size_t outlen = converted.empty() ? len : converted.size();

  if (!conn->wbuf.empty()) {
    // Already buffering — append
    conn->wbuf.insert(conn->wbuf.end(), out, out + outlen);
    return len;
  }

  int n = ssh_channel_write(conn->channel, out, outlen);
  if (n >= 0) {
    if (static_cast<size_t>(n) < outlen) {
      conn->wbuf.insert(conn->wbuf.end(), out + n, out + outlen);
      // Enable EV_WRITE
      event_del(conn->ev);
      event_assign(conn->ev, conn->base, event_get_fd(conn->ev),
                   EV_READ | EV_WRITE | EV_PERSIST, ssh_event_cb, conn);
      event_add(conn->ev, nullptr);
    }
    return len;
  }

  // SSH_AGAIN — buffer all
  conn->wbuf.insert(conn->wbuf.end(), out, out + outlen);
  event_del(conn->ev);
  event_assign(conn->ev, conn->base, event_get_fd(conn->ev),
               EV_READ | EV_WRITE | EV_PERSIST, ssh_event_cb, conn);
  event_add(conn->ev, nullptr);
  return len;
}

void ssh_conn_flush_writes(ssh_conn *conn) {
  if (conn && conn->state == SSH_STATE_READY) {
    ssh_flush_writes(conn);
  }
}

bool ssh_conn_is_eof(ssh_conn *conn) {
  return conn && conn->channel && ssh_channel_is_eof(conn->channel);
}

ssh_conn_state ssh_conn_get_state(ssh_conn *conn) {
  return conn ? conn->state : SSH_STATE_ERROR;
}
