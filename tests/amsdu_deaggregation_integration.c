/*
 * SPDX-License-Identifier: GPL-2.0
 *
 * Host-side integration test for the actual NXP A-MSDU deaggregation code.
 *
 * The runner applies patches/nxp-mlan-amsdu-length.patch to a temporary copy
 * of mlan_11n_aggr.c, then compiles that copy with this file under ASan and
 * UBSan.  These callbacks are intentionally minimal: they model the parts of
 * the MOAL interface reached by wlan_11n_deaggregate_pkt().  This is not a
 * macOS-kernel or hardware test; it makes malformed packet-boundary errors
 * deterministic in a userspace sanitizer.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mlan.h"
#include "mlan_join.h"
#include "mlan_util.h"
#include "mlan_fw.h"
#include "mlan_main.h"
#include "mlan_wmm.h"
#include "mlan_11n.h"
#include "mlan_11n_aggr.h"

/* Avoid the NXP callback macro names while implementing the host callbacks. */
#undef memcpy
#undef memmove
#undef memcmp

t_u32 mlan_drvdbg = 0;
t_void (*print_callback)(t_pvoid, t_u32, char *, ...) = NULL;
mlan_status (*get_sys_time_callback)(t_void *, t_u32 *, t_u32 *) = NULL;

pmlan_buffer wlan_alloc_mlan_buffer(mlan_adapter *adapter, t_u32 length,
                                    t_u32 headroom, t_u32 flags)
{
    pmlan_buffer buffer;

    (void)adapter;
    (void)headroom;
    (void)flags;
    buffer = calloc(1, sizeof(*buffer));
    assert(buffer);
    buffer->pbuf = calloc(1, length + 64);
    assert(buffer->pbuf);
    buffer->data_len = length;
    return buffer;
}

t_void wlan_free_mlan_buffer(mlan_adapter *adapter, pmlan_buffer buffer)
{
    (void)adapter;
    free(buffer->pbuf);
    free(buffer);
}

mlan_status wlan_recv_packet_complete(pmlan_adapter adapter,
                                      pmlan_buffer buffer, mlan_status status)
{
    wlan_free_mlan_buffer(adapter, buffer);
    return status;
}

t_bool wlan_secure_add(t_void *input, t_s32 increment, t_void *output,
                       data_type type)
{
    assert(type == TYPE_PTR);
    *(unsigned char **)output = *(unsigned char **)input + increment;
    return MTRUE;
}

mlan_status wlan_prepare_cmd(pmlan_private priv, t_u16 command, t_u16 action,
                             t_u32 oid, t_void *data, t_void *buffer)
{
    (void)priv;
    (void)command;
    (void)action;
    (void)oid;
    (void)data;
    (void)buffer;
    return MLAN_STATUS_FAILURE;
}

mlan_status wlan_recv_event(pmlan_private priv, mlan_event_id event,
                            t_void *event_data)
{
    (void)priv;
    (void)event;
    (void)event_data;
    return MLAN_STATUS_SUCCESS;
}

void wlan_process_tdls_action_frame(pmlan_private priv, t_u8 *buffer,
                                    t_u32 length)
{
    (void)priv;
    (void)buffer;
    (void)length;
}

sta_node *wlan_get_station_entry(mlan_private *priv, t_u8 *mac)
{
    (void)priv;
    (void)mac;
    return NULL;
}

mlan_status wlan_uap_recv_packet(mlan_private *priv, pmlan_buffer buffer)
{
    (void)priv;
    (void)buffer;
    return MLAN_STATUS_FAILURE;
}

static void *copy_callback(void *handle, void *destination, const void *source,
                           t_u32 length)
{
    (void)handle;
    return memcpy(destination, source, length);
}

static void *bounded_copy_callback(void *handle, void *destination,
                                   const void *source, t_u32 length,
                                   t_u32 capacity)
{
    (void)handle;
    return memcpy(destination, source, length < capacity ? length : capacity);
}

static void *move_callback(void *handle, void *destination, const void *source,
                           t_u32 length)
{
    (void)handle;
    return memmove(destination, source, length);
}

static t_s32 compare_callback(void *handle, const void *left, const void *right,
                              t_u32 length)
{
    (void)handle;
    return memcmp(left, right, length);
}

static t_u16 read_u16_callback(const void *pointer)
{
    t_u16 value;

    memcpy(&value, pointer, sizeof(value));
    return value;
}

static void write_u16_callback(void *pointer, t_u16 value)
{
    memcpy(pointer, &value, sizeof(value));
}

static unsigned int received_packets;
static unsigned int received_length;
static unsigned char received_type[2];

static mlan_status receive_callback(void *handle, pmlan_buffer buffer)
{
    unsigned char *data;

    (void)handle;
    assert(buffer->data_len >= 14);
    data = buffer->pbuf + buffer->data_offset;
    ++received_packets;
    received_length = buffer->data_len;
    received_type[0] = data[12];
    received_type[1] = data[13];
    return MLAN_STATUS_SUCCESS;
}

static mlan_status data_complete_callback(mlan_adapter *adapter,
                                          pmlan_buffer buffer,
                                          mlan_status status)
{
    (void)adapter;
    (void)buffer;
    return status;
}

static void run_one_subframe(mlan_private *priv, unsigned int total_length,
                             unsigned int declared_payload, int snap_header)
{
    mlan_buffer buffer = {0};
    unsigned char *data = calloc(1, total_length ? total_length : 1);

    assert(data);
    buffer.pbuf = data;
    buffer.data_len = total_length;
    if (total_length >= 14) {
        data[12] = (unsigned char)(declared_payload >> 8);
        data[13] = (unsigned char)declared_payload;
    }
    if (snap_header && total_length >= 22) {
        data[14] = 0xaa;
        data[15] = 0xaa;
        data[16] = 0x03;
        data[20] = 0x08;
        data[21] = 0x00;
    }

    received_packets = 0;
    wlan_11n_deaggregate_pkt(priv, &buffer);

    if (total_length < 14 || declared_payload > total_length - 14)
        assert(received_packets == 0);
    if (total_length >= 14 && declared_payload == total_length - 14) {
        assert(received_packets == 1);
        assert(received_length == (snap_header ? total_length - 8 : total_length));
        if (snap_header) {
            assert(received_type[0] == 0x08);
            assert(received_type[1] == 0x00);
        }
    }
    free(data);
}

static void run_two_subframes(mlan_private *priv, unsigned int first_payload,
                              unsigned int second_payload)
{
    const unsigned int first_frame = 14 + first_payload;
    const unsigned int padding = (4 - (first_frame & 3)) & 3;
    const unsigned int total_length = first_frame + padding + 14 + second_payload;
    unsigned char *data = calloc(1, total_length);
    unsigned char *second;
    mlan_buffer buffer = {0};

    assert(data);
    buffer.pbuf = data;
    buffer.data_len = total_length;
    data[12] = (unsigned char)(first_payload >> 8);
    data[13] = (unsigned char)first_payload;
    second = data + first_frame + padding;
    second[12] = (unsigned char)(second_payload >> 8);
    second[13] = (unsigned char)second_payload;
    if (first_payload >= 8) {
        data[14] = 0xaa;
        data[15] = 0xaa;
        data[16] = 0x03;
        data[20] = 0x08;
    }
    if (second_payload >= 8) {
        second[14] = 0xaa;
        second[15] = 0xaa;
        second[16] = 0x03;
        second[20] = 0x08;
    }

    received_packets = 0;
    wlan_11n_deaggregate_pkt(priv, &buffer);
    assert(received_packets == 2);
    free(data);
}

static void initialize(mlan_adapter *adapter, mlan_private *priv)
{
    adapter->callbacks.moal_memcpy = copy_callback;
    adapter->callbacks.moal_memcpy_ext = bounded_copy_callback;
    adapter->callbacks.moal_memmove = move_callback;
    adapter->callbacks.moal_memcmp = compare_callback;
    adapter->callbacks.moal_recv_packet = receive_callback;
    adapter->callbacks.moal_unaligned_access.moal_read_u16 = read_u16_callback;
    adapter->callbacks.moal_unaligned_access.moal_write_u16 = write_u16_callback;
    adapter->ops.data_complete = data_complete_callback;
    adapter->rx_buf_size = 4096;
    priv->adapter = adapter;
    priv->bss_role = MLAN_BSS_ROLE_STA;
}

int main(int argc, char **argv)
{
    mlan_adapter adapter = {0};
    mlan_private priv = {0};
    unsigned int total_length;
    unsigned int declared_payload;
    unsigned int payload;
    unsigned int first_payload;
    unsigned int second_payload;

    initialize(&adapter, &priv);
    if (argc == 2 && strcmp(argv[1], "--malformed-boundary") == 0) {
        /* A 14-byte frame claiming a 14-byte payload has no safe payload. */
        run_one_subframe(&priv, 14, 14, 0);
        puts("Malformed A-MSDU boundary rejected.");
        return 0;
    }
    assert(argc == 1);

    for (total_length = 0; total_length <= 128; ++total_length) {
        for (declared_payload = 0;
             declared_payload <= total_length + 16;
             ++declared_payload)
            run_one_subframe(&priv, total_length, declared_payload, 0);
        run_one_subframe(&priv, total_length, 65535, 0);
    }
    for (payload = 8; payload <= 128; ++payload)
        run_one_subframe(&priv, 14 + payload, payload, 1);
    for (first_payload = 0; first_payload < 32; ++first_payload) {
        for (second_payload = 0; second_payload < 32; ++second_payload)
            run_two_subframes(&priv, first_payload, second_payload);
    }
    puts("Patched NXP A-MSDU deaggregation: 12,000+ single inputs and "
         "1,024 two-subframe inputs passed.");
    return 0;
}
