/* Exercise the guard compiled into both NXP A-MSDU receive passes. */
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include "../patches/marvell_amsdu_guard.h"

int main(void)
{
    const unsigned int header = 14;
    for (unsigned int remaining = 0; remaining <= 4096; ++remaining) {
        for (unsigned int payload = 0; payload <= 4096; ++payload) {
            unsigned int padding = UINT_MAX;
            const int accepted = marvell_amsdu_padding(remaining, payload,
                                                       header, &padding);
            if (remaining < header || payload > remaining - header) {
                assert(!accepted);
                continue;
            }
            const unsigned int frame = header + payload;
            const unsigned int expected = frame == remaining ? 0 : (4 - (frame & 3)) & 3;
            if (expected > remaining - frame) {
                assert(!accepted);
            } else {
                assert(accepted);
                assert(padding == expected && frame + padding <= remaining);
                if (frame < remaining) assert(((frame + padding) & 3) == 0);
            }
        }
    }
    unsigned int padding = UINT_MAX;
    assert(marvell_amsdu_padding(15, 1, 14, &padding) && padding == 0);
    assert(!marvell_amsdu_padding(15, 0, 14, &padding));
    assert(marvell_amsdu_padding(18, 1, 14, &padding) && padding == 1);
    assert(!marvell_amsdu_padding(UINT_MAX, UINT_MAX, 14, &padding));
    assert(!marvell_amsdu_padding(20, 0, 14, 0));
    puts("A-MSDU guard: 16,785,409 length pairs and edge cases passed.");
    return 0;
}
