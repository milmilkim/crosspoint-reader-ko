#!/usr/bin/env python3
"""Print fontconvert.py --additional-intervals args for the KS X 1001 Hanja set.

KS X 1001 (EUC-KR "wansung") carries 4,888 Hanja in rows 0xCA..0xFD, which covers
the Hanja found in Korean prose. The built-in reader font (Bookk Myungjo) ships
exactly these; anything rarer is drawn from the SD-card font it falls back to.

Usage:
    python3 ks_x_1001_hanja_intervals.py            # "--additional-intervals X,Y" tokens
    python3 ks_x_1001_hanja_intervals.py --count    # just report the totals
"""

import sys


def hanja_codepoints():
    cps = set()
    for lead in range(0xCA, 0xFE):
        for trail in range(0xA1, 0xFF):
            try:
                ch = bytes((lead, trail)).decode('euc_kr')
            except UnicodeDecodeError:
                continue
            cp = ord(ch)
            if 0x4E00 <= cp <= 0x9FFF or 0xF900 <= cp <= 0xFAFF:
                cps.add(cp)
    return sorted(cps)


def runs(cps):
    out = []
    for cp in cps:
        if out and out[-1][1] == cp - 1:
            out[-1][1] = cp
        else:
            out.append([cp, cp])
    return out


if __name__ == '__main__':
    cps = hanja_codepoints()
    if '--count' in sys.argv[1:]:
        print(f'{len(cps)} Hanja in {len(runs(cps))} runs')
    else:
        print(' '.join(f'--additional-intervals 0x{a:04X},0x{b:04X}' for a, b in runs(cps)))
