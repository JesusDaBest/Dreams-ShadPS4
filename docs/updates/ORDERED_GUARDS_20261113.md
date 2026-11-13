# Guarded ordered DOC — November 13, 2026

**[Download the graphical package](https://github.com/JesusDaBest/Dreams-ShadPS4/raw/refs/heads/main/builds/cusa04301-ordered-guards-20261113/Dreams-graphical-launcher-ordered-guards-20261113.zip)**

Extract the ZIP and open **shadPS4QtLauncher.exe**. No commands or extra setup.

Adds verified collect → exclusive prefix → replay support for `fff20d00` indirect dispatches, guarded `2bfebd3c` direct dispatches, and `252ccde6` direct dispatches only when a coherent per-dispatch alias proof passes. Unsupported or uncertain cases retain atomic fallback. Existing ordered paths and all previous fixes remain.

Full captured-input GPU replay and live phase activation were verified for all three. Exclusive prefixes, tokens and unrelated GDS contents were checked. A deliberate overlap decision rejected the ordered route. For `252ccde6`, immutable GPU input snapshots and synchronous index/counter readback remain; no speedup is claimed.

The clean release excludes the private capture UI, learner hooks and authoring connection. The corresponding private diagnostic build is retained. No saves or game files are bundled.

Source: [patch](../../patches/dreams-ordered-guards-20261113.patch) · [complete source manifest](../../builds/cusa04301-ordered-guards-20261113/source-manifest.json) · [previous update](DOC_PREFIX_20261112.md).
