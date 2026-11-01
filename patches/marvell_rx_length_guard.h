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

/*
 * The PCIe guard proves the fixed descriptor itself exists.  After decoding
 * it, the firmware still controls the payload offset and length, and may ask
 * the host to decode an optional RxPD extension.  Check every range with
 * subtraction before a caller creates or dereferences either pointer.
 */
static inline int marvell_valid_rxpd_layout(unsigned int buffer_length,
                                            unsigned int descriptor_length,
                                            unsigned int extension_length,
                                            unsigned int has_extension,
                                            unsigned int payload_offset,
                                            unsigned int payload_length)
{
    if (descriptor_length > buffer_length)
        return 0;
    if (has_extension && extension_length > buffer_length - descriptor_length)
        return 0;
    if (payload_offset > buffer_length ||
        payload_length > buffer_length - payload_offset)
        return 0;
    return payload_offset + payload_length == buffer_length;
}

#endif
