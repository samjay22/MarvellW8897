/* SPDX-License-Identifier: GPL-2.0 */
/* Bound an 802.11 A-MSDU subframe including its header and alignment. */
#ifndef MARVELL_AMSDU_GUARD_H
#define MARVELL_AMSDU_GUARD_H

static inline int marvell_amsdu_padding(unsigned int remaining,
                                       unsigned int payload_length,
                                       unsigned int header_length,
                                       unsigned int *padding)
{
    unsigned int frame_length;
    unsigned int pad;
    if (!padding || header_length > remaining ||
        payload_length > remaining - header_length)
        return 0;
    frame_length = header_length + payload_length;
    /* The final subframe needs no alignment padding. */
    pad = frame_length == remaining ? 0U : ((4U - (frame_length & 3U)) & 3U);
    if (pad > remaining - frame_length)
        return 0;
    *padding = pad;
    return 1;
}

#endif
