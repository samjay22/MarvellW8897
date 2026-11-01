/* Test the exact guard included in the patched MLAN PCIe receive path. */
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include "../patches/marvell_rx_length_guard.h"

int main(void)
{
    const unsigned int transport = 4, descriptor = 24, capacity = 4096;
    for (unsigned int length = 0; length <= 65535; ++length) {
        int accepted = marvell_valid_pcie_rx_length(length, capacity,
                                                   transport, descriptor);
        if (length < transport + descriptor || length > capacity)
            assert(!accepted);
        else
            assert(accepted);
        if (accepted) {
            assert(length >= transport);
            assert(length - transport >= descriptor);
        }
    }
    assert(!marvell_valid_pcie_rx_length(3, UINT_MAX, 4, 0));
    assert(!marvell_valid_pcie_rx_length(UINT_MAX, UINT_MAX, UINT_MAX, 1));
    assert(marvell_valid_pcie_rx_length(UINT_MAX, UINT_MAX, 0, UINT_MAX));
    assert(!marvell_valid_pcie_rx_length(100, 99, 4, 24));

    /* Cover every descriptor/payload boundary in a normal 4 KiB RX buffer. */
    for (unsigned int total = 0; total <= 4096; ++total) {
        for (unsigned int offset = 0; offset <= 4096; ++offset) {
            const unsigned int payload = offset <= total ? total - offset : 0;
            const int accepted = marvell_valid_rxpd_layout(
                total, 48, 36, 0, offset, payload);
            if (total < 48 || offset > total)
                assert(!accepted);
            else
                assert(accepted);

            assert(!marvell_valid_rxpd_layout(
                total, 48, 36, 0, offset, payload + 1));
            if (total >= 48) {
                assert(marvell_valid_rxpd_layout(
                    total, 48, total - 48, 1, total, 0));
                assert(!marvell_valid_rxpd_layout(
                    total, 48, total - 48 + 1, 1, total, 0));
            }
        }
    }
    assert(!marvell_valid_rxpd_layout(UINT_MAX, 1, 0, 0, UINT_MAX, 1));
    assert(!marvell_valid_rxpd_layout(UINT_MAX, 1, 0, 0, 1, UINT_MAX));
    puts("PCIe RX length guard: all 65,536 firmware length values, 16M RxPD layout boundaries, and overflow boundaries passed.");
    return 0;
}
