/*
 *
 * Copyright 2021-2023 Software Radio Systems Limited
 *
 * This file is part of srsRAN.
 *
 * srsRAN is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 *
 * srsRAN is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * A copy of the GNU Affero General Public License can be found in
 * the LICENSE file in the top-level directory of this distribution
 * and at http://www.gnu.org/licenses/.
 *
 */

#include "dpdk_ethernet_transmitter.h"
#include "srsran/adt/static_vector.h"
#include "srsran/ofh/ethernet/ethernet_gw_config.h"
#include <rte_ethdev.h>

using namespace srsran;
using namespace ether;

/// DPDK configuration settings.
static constexpr unsigned MAX_BURST_SIZE  = 32;
static constexpr unsigned MAX_BUFFER_SIZE = 9600;
static constexpr unsigned MBUF_CACHE_SIZE = 250;
static constexpr unsigned RX_RING_SIZE    = 1024;
static constexpr unsigned TX_RING_SIZE    = 1024;
static constexpr unsigned NUM_MBUFS       = 8191;

/// DPDK port initialization routine.
static bool port_init(const gw_config& config, ::rte_mempool* mbuf_pool, unsigned port)
{
  uint16_t nb_rxd = RX_RING_SIZE;
  uint16_t nb_txd = TX_RING_SIZE;

  if (::rte_eth_dev_is_valid_port(port) == 0) {
    fmt::print("Invalid port={}\n", port);
    return false;
  }

  ::rte_eth_dev_info dev_info;
  int                ret = ::rte_eth_dev_info_get(port, &dev_info);
  if (ret != 0) {
    fmt::print("Error during getting device (port {}) info: {}\n", port, ::strerror(-ret));
    return false;
  }

  ::rte_eth_conf port_conf = {};
  if (dev_info.tx_offload_capa & RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE) {
    port_conf.txmode.offloads |= RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE;
  }

  // Configure the Ethernet device.
  if (::rte_eth_dev_configure(port, 1, 1, &port_conf) != 0) {
    fmt::print("Error configuring eth dev\n");
    return false;
  }

  // Configure MTU size.
  if (::rte_eth_dev_set_mtu(port, config.mtu_size.value()) != 0) {
    uint16_t current_mtu;
    ::rte_eth_dev_get_mtu(port, &current_mtu);
    fmt::print(
        "Unable to set MTU size = {} bytes for NIC interface in the Ethernet transmitter, current MTU = {} bytes\n",
        config.mtu_size,
        current_mtu);
    return false;
  }

  if (::rte_eth_dev_adjust_nb_rx_tx_desc(port, &nb_rxd, &nb_txd) != 0) {
    fmt::print("Error configuring eth number of tx/rx descriptors\n");
    return false;
  }

  // Allocate and set up 1 RX queue.
  if (::rte_eth_rx_queue_setup(port, 0, nb_rxd, ::rte_eth_dev_socket_id(port), nullptr, mbuf_pool) < 0) {
    fmt::print("Error configuring rx queue\n");
    return false;
  }

  ::rte_eth_txconf txconf = dev_info.default_txconf;
  txconf.offloads         = port_conf.txmode.offloads;
  // Allocate and set up 1 TX queue.
  if (::rte_eth_tx_queue_setup(port, 0, nb_txd, ::rte_eth_dev_socket_id(port), &txconf) < 0) {
    fmt::print("Error configuring tx queue\n");
    return false;
  }

  // Start Ethernet port.
  if (::rte_eth_dev_start(port) < 0) {
    fmt::print("Error starting dev\n");
    return false;
  }

  // Enable RX in promiscuous mode for the Ethernet device.
  if (config.is_promiscuous_mode_enabled) {
    if (::rte_eth_promiscuous_enable(port) != 0) {
      fmt::print("Error enabling promiscuous mode\n");
      return false;
    }
  }

  return true;
}

/// Configures an Ethernet port using DPDK.
static void dpdk_port_configure(const gw_config& config, ::rte_mempool*& mbuf_pool)
{
  if (::rte_eth_dev_count_avail() != 1) {
    ::rte_exit(
        EXIT_FAILURE, "Error: number of DPDK devices must be one but is currently %d\n", ::rte_eth_dev_count_avail());
  }

  // Creates a new mempool in memory to hold the mbufs.
  mbuf_pool =
      ::rte_pktmbuf_pool_create("OFH_MBUF_POOL", NUM_MBUFS, MBUF_CACHE_SIZE, 0, MAX_BUFFER_SIZE, ::rte_socket_id());
  if (mbuf_pool == nullptr) {
    ::rte_exit(EXIT_FAILURE, "Cannot create mbuf pool\n");
  }

  // Initializing all ports (single one for now).
  unsigned portid;
  RTE_ETH_FOREACH_DEV(portid)
  {
    if (!port_init(config, mbuf_pool, portid)) {
      ::rte_exit(EXIT_FAILURE, "Cannot init port\n");
    }
  }
}

void dpdk_transmitter_impl::send(span<span<const uint8_t>> frames)
{
  if (frames.size() >= MAX_BURST_SIZE) {
    logger.warning("Unable to send a transmission burst size of '{}' frames in the DPDK Ethernet transmitter",
                   frames.size());
    return;
  }

  static_vector<::rte_mbuf*, MAX_BURST_SIZE> mbufs(frames.size());
  if (::rte_pktmbuf_alloc_bulk(mbuf_pool, mbufs.data(), frames.size()) < 0) {
    logger.warning("Not enough entries in the mempool to send '{}' frames in the DPDK Ethernet transmitter",
                   frames.size());
    return;
  }

  for (unsigned idx = 0, end = frames.size(); idx != end; ++idx) {
    const auto  frame = frames[idx];
    ::rte_mbuf* mbuf  = mbufs[idx];

    if (::rte_pktmbuf_append(mbuf, frame.size()) == nullptr) {
      ::rte_pktmbuf_free(mbuf);
      logger.warning("Unable to append '{}' bytes to the allocated mbuf in the DPDK Ethernet transmitter",
                     frame.size());
      ::rte_pktmbuf_free_bulk(mbufs.data(), mbufs.size());
      return;
    }
    mbuf->data_len = frame.size();
    mbuf->pkt_len  = frame.size();

    uint8_t* data = rte_pktmbuf_mtod(mbuf, uint8_t*);
    std::memcpy(data, frame.data(), frame.size());
  }

  unsigned nof_sent_packets = ::rte_eth_tx_burst(port_id, 0, mbufs.data(), mbufs.size());

  if (SRSRAN_UNLIKELY(nof_sent_packets < mbufs.size())) {
    logger.warning("DPDK dropped '{}' packets out of a total of '{}' in the tx burst",
                   mbufs.size() - nof_sent_packets,
                   mbufs.size());
    for (unsigned buf_idx = nof_sent_packets, last_idx = mbufs.size(); buf_idx != last_idx; ++buf_idx) {
      ::rte_pktmbuf_free(mbufs[buf_idx]);
    }
  }
}

dpdk_transmitter_impl::dpdk_transmitter_impl(const gw_config& config, srslog::basic_logger& logger_) : logger(logger_)
{
  dpdk_port_configure(config, mbuf_pool);
}
