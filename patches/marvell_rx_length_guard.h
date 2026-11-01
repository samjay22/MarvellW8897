/* SPDX-License-Identifier: GPL-2.0 */
/* Validate a PCIe RX data length before removing the transport header. */
#ifndef MARVELL_RX_LENGTH_GUARD_H
#define MARVELL_RX_LENGTH_GUARD_H

static inline int marvell_valid_pcie_rx_length(unsigned int length,
                                              unsigned int capacity,
                                              unsigned int transport_header,
                                              unsigned int rx_descriptor)
{
    return length <= capacity && length >= transport_header &&
           length - transport_header >= rx_descriptor;
}

#endif
