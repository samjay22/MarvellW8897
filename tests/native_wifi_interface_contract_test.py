#!/usr/bin/env python3
"""Guard the public shape of the Wi-Fi-only kext.

This is deliberately a source-level contract, not a substitute for a boot test:
Apple's IO80211 implementation is a private kernel ABI and must still be tested
on the target Surface.  The test prevents accidental regression to the older
Ethernet-style publication path while retaining compile-time verification in
the Xcode kext build.
"""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
CONTROLLER_HEADER = (ROOT / "Contents/MacOS/MarvellW8897Controller.hpp").read_text()
CONTROLLER_SOURCE = (ROOT / "Contents/MacOS/MarvellW8897Controller.cpp").read_text()
INFO_PLIST = (ROOT / "Contents/info.plist").read_text()


def require(text: str, source: str, description: str) -> None:
    if text not in source:
        raise AssertionError(f"missing {description}: {text!r}")


def forbid(text: str, source: str, description: str) -> None:
    if text in source:
        raise AssertionError(f"forbidden {description}: {text!r}")


require("class MarvellW8897Controller : public IO80211Controller",
        CONTROLLER_HEADER, "IO80211 controller inheritance")
require("IO80211Interface*", CONTROLLER_HEADER, "IO80211 interface member")
require("kIOMediumIEEE80211Auto", CONTROLLER_SOURCE, "Wi-Fi medium")
require("APPLE80211_M_SCAN_DONE", CONTROLLER_SOURCE, "scan completion notification")
require("APPLE80211_M_LINK_CHANGED", CONTROLLER_SOURCE, "link-state notification")
require("APPLE80211_IOC_ASSOCIATE", CONTROLLER_SOURCE, "native association request")
require("association->ad_key.key_len == 32", CONTROLLER_SOURCE,
        "bounded PMK association path")
require("com.apple.iokit.IO80211Family", INFO_PLIST, "IO80211 dependency")

forbidden_paths = (CONTROLLER_HEADER, CONTROLLER_SOURCE)
for source in forbidden_paths:
    forbid("IOEthernetController", source, "Ethernet controller path")
    forbid("IOEthernetInterface", source, "Ethernet interface path")
    forbid("kIOMediumEthernet", source, "Ethernet medium")

print("Native Wi-Fi interface contract passed: IO80211-only publication path retained.")
