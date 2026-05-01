/*
 * Copyright (c) 2026 [大河马/dahema@me.com]
 * SPDX-License-Identifier: MIT
 */

#ifndef NET_SSH_H
#define NET_SSH_H

#include <event2/util.h>

struct event_base;
struct interactive_t;

// Opaque SSH handles (actual types from libssh, forward-declared to avoid
// macro conflicts with base/internal/stralloc.h)
struct ssh_bind_struct;
struct ssh_conn;

// SSH connection state machine
enum ssh_conn_state {
  SSH_STATE_HANDSHAKE,
  SSH_STATE_AUTH,
  SSH_STATE_CHANNEL,
  SSH_STATE_READY,
  SSH_STATE_ERROR
};

// Server lifecycle
struct ssh_bind_struct *ssh_server_init(const char *config_dir);
void ssh_server_close(struct ssh_bind_struct *bind);

// Per-connection lifecycle (called on the owning IO thread)
ssh_conn *ssh_conn_accept(struct ssh_bind_struct *bind, evutil_socket_t fd,
                          event_base *base, interactive_t *user);
void ssh_conn_free(ssh_conn *conn);

// I/O (called on the IO thread)
int ssh_conn_write(ssh_conn *conn, const char *buf, size_t len);
bool ssh_conn_is_eof(ssh_conn *conn);
ssh_conn_state ssh_conn_get_state(ssh_conn *conn);

void ssh_conn_flush_writes(ssh_conn *conn);

#endif  // NET_SSH_H
