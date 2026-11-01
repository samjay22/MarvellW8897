/*
 * Test-only QEMU 8.2 PCI model for the Surface Pro 4's Marvell 88W8897.
 *
 * This models PCI discovery and the firmware helper's polling/DMA handshake.
 * It is deliberately not an implementation of the firmware, MLAN commands,
 * radio, or Wi-Fi association. Never use this as evidence of RF operation.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "hw/pci/pci.h"
#include "hw/pci/msi.h"
#include "hw/pci/pcie.h"
#include "hw/qdev-properties.h"
#include "qapi/error.h"
#include "qom/object.h"
#include "qemu/module.h"
#include "qemu/log.h"

#define TYPE_MARVELL_8897_TEST "marvell-8897-test"
typedef struct Marvell8897TestState Marvell8897TestState;
DECLARE_INSTANCE_CHECKER(Marvell8897TestState, MARVELL_8897_TEST,
                         TYPE_MARVELL_8897_TEST)

enum {
    FW_IMAGE_SIZE = 723540,
    HELPER_BLOCK_SIZE = 1024,
    FW_DOWNLOAD_BLOCK_SIZE = 256,
    FW_BLOCKS = (FW_IMAGE_SIZE + HELPER_BLOCK_SIZE - 1) / HELPER_BLOCK_SIZE,
    FW_LAST_DATA = FW_IMAGE_SIZE % HELPER_BLOCK_SIZE,
    FW_LAST_DMA = (FW_LAST_DATA + FW_DOWNLOAD_BLOCK_SIZE - 1) /
                  FW_DOWNLOAD_BLOCK_SIZE * FW_DOWNLOAD_BLOCK_SIZE,
    FW_READY = 0xfedcba00,
};

struct Marvell8897TestState {
    PCIDevice parent_obj;
    MemoryRegion bar0;
    MemoryRegion bar2;
    uint64_t dma_address;
    uint32_t dma_length;
    uint32_t blocks;
    uint32_t firmware_status;
    uint32_t driver_ready;
    uint32_t host_interrupt_mask;
    uint32_t host_interrupt_status_mask;
    bool first_block_valid;
    bool final_padding_valid;
    bool firmware_matches;
    char *firmware_path;
    gchar *expected_firmware;
};

static uint64_t marvell_bar0_read(void *opaque, hwaddr offset, unsigned size)
{
    return 0;
}

static void marvell_bar0_write(void *opaque, hwaddr offset,
                               uint64_t value, unsigned size)
{
    qemu_log_mask(LOG_GUEST_ERROR,
                  "marvell-8897-test: unexpected BAR0 write at 0x%" HWADDR_PRIx "\n",
                  offset);
}

static uint64_t marvell_bar2_read(void *opaque, hwaddr offset, unsigned size)
{
    Marvell8897TestState *s = opaque;

    switch (offset) {
    case 0x0c40: /* helper's next block request */
        return s->blocks < FW_BLOCKS ? HELPER_BLOCK_SIZE : 0;
    case 0x0c44: /* firmware status */
        return s->firmware_status;
    case 0x0cf0: /* driver ready */
        return s->driver_ready;
    case 0x0c1c: /* CPU interrupt status: doorbell already acknowledged */
        return 0;
    case 0x0c34:
        return s->host_interrupt_mask;
    case 0x0c3c:
        return s->host_interrupt_status_mask;
    default:
        return 0;
    }
}

static void marvell_receive_firmware_block(Marvell8897TestState *s)
{
    uint8_t block[HELPER_BLOCK_SIZE];
    uint32_t expected_length = s->blocks == FW_BLOCKS - 1
        ? FW_LAST_DMA : HELPER_BLOCK_SIZE;

    if (s->blocks >= FW_BLOCKS || s->dma_length != expected_length ||
        s->dma_address > UINT32_MAX ||
        !(pci_get_word(s->parent_obj.config + PCI_COMMAND) & PCI_COMMAND_MASTER)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "marvell-8897-test: invalid firmware DMA block %u len=%u addr=0x%" PRIx64 "\n",
                      s->blocks, s->dma_length, s->dma_address);
        return;
    }

    if (pci_dma_read(&s->parent_obj, s->dma_address,
                     block, expected_length) != MEMTX_OK) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "marvell-8897-test: firmware DMA read failed at block %u\n",
                      s->blocks);
        return;
    }

    size_t image_offset = (size_t)s->blocks * HELPER_BLOCK_SIZE;
    size_t data_length = MIN((size_t)HELPER_BLOCK_SIZE,
                             (size_t)FW_IMAGE_SIZE - image_offset);
    if (memcmp(block, s->expected_firmware + image_offset, data_length)) {
        s->firmware_matches = false;
        qemu_log_mask(LOG_GUEST_ERROR,
                      "marvell-8897-test: firmware mismatch at block %u\n",
                      s->blocks);
    }

    if (s->blocks == 0) {
        s->first_block_valid = block[0] == 1 && block[1] == 0 &&
                               block[2] == 0 && block[3] == 0;
    }
    if (s->blocks == FW_BLOCKS - 1) {
        s->final_padding_valid = true;
        for (unsigned i = FW_LAST_DATA; i < expected_length; ++i) {
            if (block[i] != 0) s->final_padding_valid = false;
        }
    }
    ++s->blocks;
}

static void marvell_bar2_write(void *opaque, hwaddr offset,
                                uint64_t value, unsigned size)
{
    Marvell8897TestState *s = opaque;

    switch (offset) {
    case 0x0c10:
        s->dma_address = (s->dma_address & 0xffffffff00000000ULL) |
                         (uint32_t)value;
        break;
    case 0x0c14:
        s->dma_address = (s->dma_address & 0x00000000ffffffffULL) |
                         ((uint64_t)(uint32_t)value << 32);
        break;
    case 0x0c40:
        s->dma_length = (uint32_t)value;
        break;
    case 0x0c18:
        if (value & (1U << 1)) marvell_receive_firmware_block(s);
        break;
    case 0x0c34:
        s->host_interrupt_mask = (uint32_t)value;
        break;
    case 0x0c3c:
        s->host_interrupt_status_mask = (uint32_t)value;
        break;
    case 0x0cf0:
        s->driver_ready = (uint32_t)value;
        if (value == FW_READY && s->blocks == FW_BLOCKS &&
            s->first_block_valid && s->final_padding_valid &&
            s->firmware_matches) {
            s->firmware_status = FW_READY;
            qemu_log_mask(LOG_GUEST_ERROR,
                          "marvell-8897-test: accepted %u firmware DMA blocks\n",
                          s->blocks);
        } else if (value == FW_READY) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "marvell-8897-test: rejected firmware blocks=%u first=%d padding=%d image=%d\n",
                          s->blocks, s->first_block_valid,
                          s->final_padding_valid, s->firmware_matches);
        }
        break;
    default:
        break;
    }
}

static const MemoryRegionOps marvell_bar0_ops = {
    .read = marvell_bar0_read,
    .write = marvell_bar0_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static const MemoryRegionOps marvell_bar2_ops = {
    .read = marvell_bar2_read,
    .write = marvell_bar2_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void marvell_test_realize(PCIDevice *pdev, Error **errp)
{
    Marvell8897TestState *s = MARVELL_8897_TEST(pdev);
    uint8_t *config = pdev->config;
    gsize firmware_size = 0;

    if (!s->firmware_path ||
        !g_file_get_contents(s->firmware_path, &s->expected_firmware,
                             &firmware_size, NULL) ||
        firmware_size != FW_IMAGE_SIZE) {
        error_setg(errp, "marvell-8897-test requires firmware= pointing to the %d-byte pcie8897_uapsta.bin", FW_IMAGE_SIZE);
        return;
    }
    s->firmware_matches = true;

    pci_config_set_interrupt_pin(config, 1);
    if (pci_add_capability(pdev, PCI_CAP_ID_PM, 0x40, PCI_PM_SIZEOF, errp) < 0) {
        return;
    }
    pci_set_word(config + 0x42, 0xdfc3); /* PM v3/D1/D2, observed in Linux */
    pci_set_word(config + 0x44, 0x0008); /* D0, NoSoftRst */
    if (msi_init(pdev, 0x50, 1, true, true, errp)) {
        return;
    }
    if (pcie_endpoint_cap_init(pdev, 0x70) < 0) {
        error_setg(errp, "marvell-8897-test could not add PCIe capability");
        return;
    }

    memory_region_init_io(&s->bar0, OBJECT(s), &marvell_bar0_ops, s,
                          "marvell-8897-test-bar0", MiB);
    memory_region_init_io(&s->bar2, OBJECT(s), &marvell_bar2_ops, s,
                          "marvell-8897-test-bar2", MiB);
    pci_register_bar(pdev, 0,
                     PCI_BASE_ADDRESS_MEM_TYPE_64 | PCI_BASE_ADDRESS_MEM_PREFETCH,
                     &s->bar0);
    pci_register_bar(pdev, 2,
                     PCI_BASE_ADDRESS_MEM_TYPE_64 | PCI_BASE_ADDRESS_MEM_PREFETCH,
                     &s->bar2);
}

static void marvell_test_exit(PCIDevice *pdev)
{
    Marvell8897TestState *s = MARVELL_8897_TEST(pdev);

    g_free(s->expected_firmware);
    pcie_cap_exit(pdev);
    msi_uninit(pdev);
}

static Property marvell_test_properties[] = {
    DEFINE_PROP_STRING("firmware", Marvell8897TestState, firmware_path),
    DEFINE_PROP_END_OF_LIST(),
};

static void marvell_test_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->realize = marvell_test_realize;
    k->exit = marvell_test_exit;
    k->vendor_id = 0x11ab;
    k->device_id = 0x2b38;
    k->subsystem_vendor_id = 0x0004;
    k->subsystem_id = 0x045e;
    k->revision = 0x00;
    k->class_id = PCI_CLASS_NETWORK_ETHERNET;
    device_class_set_props(dc, marvell_test_properties);
    set_bit(DEVICE_CATEGORY_NETWORK, dc->categories);
}

static const TypeInfo marvell_test_info = {
    .name = TYPE_MARVELL_8897_TEST,
    .parent = TYPE_PCI_DEVICE,
    .instance_size = sizeof(Marvell8897TestState),
    .class_init = marvell_test_class_init,
    .interfaces = (InterfaceInfo[]) {
        { INTERFACE_PCIE_DEVICE },
        { },
    },
};

static void marvell_test_register_types(void)
{
    type_register_static(&marvell_test_info);
}
type_init(marvell_test_register_types)
