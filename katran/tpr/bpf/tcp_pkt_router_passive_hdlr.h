// (c) Facebook, Inc. and its affiliates. Confidential and proprietary.

#pragma once

#include <bpf/bpf_helpers.h>
#include <bpf/helpers/bpf_endian.h>
#include <linux/bpf.h>
#include <linux/string.h>
#include <linux/tcp.h>
#include <linux/types.h>
#include <stdbool.h>

#include "tcp_pkt_router_common.h"
#include "tcp_pkt_router_consts.h"
#include "tcp_pkt_router_maps.h"
#include "tcp_pkt_router_structs.h"

static inline int handle_passive_parse_hdr(
    struct bpf_sock_ops* skops,
    struct stats* stat,
    const struct server_info* s_info) {
  int err;
  struct tcp_opt hdr_opt = {};

  hdr_opt.kind = TCP_HDR_OPT_KIND;
  err = bpf_load_hdr_opt(skops, &hdr_opt, sizeof(hdr_opt), NO_FLAGS);
  if (err < 0) {
    // peer didn't write anything.
    stat->no_tcp_opt_hdr++;
    return err;
  }
  if (!hdr_opt.server_id) {
    // no server_id received from peer.
    stat->error_bad_id++;
    return PASS;
  }
  if (s_info->server_id != hdr_opt.server_id) {
    // read the server_id. But not itself. Packet is misrouted.
    stat->error_bad_id++;
    return PASS;
  } else {
    // no need to keep writing this once peer sends the right server_id.
    stat->server_id_read++;
    err = unset_parse_hdr_cb_flags(skops, stat);
    err |= unset_write_hdr_cb_flags(skops, stat);
    return err;
  }
  return SUCCESS;
}

static inline int handle_passive_write_hdr_opt(
    struct bpf_sock_ops* skops,
    struct stats* stat,
    const struct server_info* s_info) {
  int err;
  struct tcp_opt hdr_opt = {};

  hdr_opt.kind = TCP_HDR_OPT_KIND;
  hdr_opt.len = TCP_HDR_OPT_LEN;
  hdr_opt.server_id = s_info->server_id;
  err = bpf_store_hdr_opt(skops, &hdr_opt, sizeof(hdr_opt), NO_FLAGS);
  if (err) {
    stat->error_write_opt++;
    return err;
  }
  stat->server_id_set++;
  return SUCCESS;
}

static inline int handle_passive_estab(
    struct bpf_sock_ops* skops,
    struct stats* stat,
    const struct server_info* s_info) {
  int err;
  struct tcp_opt hdr_opt = {};

  // check if received packet from peer has the right server_id
  hdr_opt.kind = TCP_HDR_OPT_KIND;
  err = bpf_load_hdr_opt(skops, &hdr_opt, sizeof(struct tcp_opt), NO_FLAGS);
  if (err < 0) {
    stat->no_tcp_opt_hdr++;
    // since the peer didn't send any header, likely it doesn't support it.
    unset_write_hdr_cb_flags(skops, stat);
    unset_parse_hdr_cb_flags(skops, stat);
    return err;
  }
  if (s_info->server_id != hdr_opt.server_id) {
    stat->error_bad_id++;
    // the peer sent the server_id but it is wrong.
    // keep on sending the server_id and reading peer's tcp-hdr
    return set_parse_hdr_cb_flags(skops, stat);
  } else {
    stat->server_id_read++;
  }
  // no need to keep writing this once the connection is established.
  err = unset_parse_hdr_cb_flags(skops, stat);
  err |= unset_write_hdr_cb_flags(skops, stat);
  return err;
}