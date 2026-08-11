# screp replay parser

This directory contains the Windows amd64 `screp` v1.13.3 command-line replay parser by Andras Belicza.

- Upstream: https://github.com/icza/screp
- Release: https://github.com/icza/screp/releases/tag/v1.13.3
- Archive: `screp-v1.13.3-windows-amd64.zip`
- Archive SHA-256: `730EB02C433EDD08A8C7AB1F614C89CC8E2B2CDFBE9D948C2063C708EEEA3D22`
- Version: v1.13.3
- Bundled executable SHA-256: `C7BBDA7C78F3129798409F652BEEFFD18383EEC2D3A9D65521C396380A4746C4`
- License: Apache License 2.0; see `LICENSE.txt`

The profiler embeds this executable as a Windows resource, extracts it to the user's local application-data tools directory when post-game replay analysis is needed, and invokes it without opening a console window. No replay parsing occurs during active recording.
