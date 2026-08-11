# screp replay parser

This directory contains the Windows amd64 `screp` v1.11.3 command-line replay parser by Andras Belicza.

- Upstream: https://github.com/icza/screp
- Version: v1.11.3
- Bundled executable SHA-256: `E3134A7E8039B3AC99EFC8A5420769BB3E819771670BEEB4F30945FB565E3E3E`
- License: Apache License 2.0; see `LICENSE.txt`

The profiler embeds this executable as a Windows resource, extracts it to the user's local application-data tools directory when post-game replay analysis is needed, and invokes it without opening a console window. No replay parsing occurs during active recording.
