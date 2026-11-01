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
    puts("PCIe RX length guard: all 65,536 firmware length values and overflow boundaries passed.");
    return 0;
}
