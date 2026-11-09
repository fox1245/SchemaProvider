# Third-Party Notices

SchemaProvider is licensed under the MIT License (see [LICENSE](LICENSE)). Depending on the build options selected at CMake configure time, SchemaProvider builds against or links the following third-party components.

## libsodium

- **Pinned version:** `1.0.22` (minimum supported version: `1.0.19`; pin recorded in [`config/dependencies.json`](config/dependencies.json))
- **Licence:** ISC License (Frank Denis `<j at pureftpd dot org>`)
- **Upstream:** `https://download.libsodium.org/libsodium/releases/`
- **Usage in SchemaProvider:** Optional dependency (`-DSP_WITH_SODIUM=ON`, or `AUTO` when found) used by the host-native archive custody backend (`sp_core`) for `sodium_init`, `sodium_malloc`, `sodium_mprotect_*`, `sodium_memzero`, `crypto_kdf_hkdf_sha256_*`, and `crypto_aead_xchacha20poly1305_ietf_*`.
- **Installed licence text:** When SchemaProvider is built with libsodium enabled, the `LICENSE` file from the unpacked pinned libsodium source archive is installed to `share/doc/SchemaProvider/third_party/libsodium/LICENSE`.

## yyjson

- **Licence:** MIT License
- **Copyright:** Copyright (c) 2020 YaoYuan `<ibireme@gmail.com>`
- **Usage in SchemaProvider:** Required JSON parser and serializer used by `sp_json` (linked from an installed `yyjson` package or compiled from a supplied `YYJSON_ROOT` source directory).

## Standalone Asio

- **Licence:** Boost Software License 1.0 (BSL-1.0)
- **Copyright:** Copyright (c) 2003-2024 Christopher M. Kohlhoff
- **Usage in SchemaProvider:** Header-only library used by `sp_transport` when `SP_BUILD_TRANSPORT=ON`. Not used when `SP_BUILD_TRANSPORT=OFF` (including WebAssembly builds).

## libcurl

- **Minimum version:** `7.88`
- **Licence:** curl License (MIT/X derivative)
- **Copyright:** Copyright (c) 1996-2026 Daniel Stenberg, `<daniel@haxx.se>`, and many contributors
- **Usage in SchemaProvider:** Shared or static system dependency linked by `sp_transport` when `SP_BUILD_TRANSPORT=ON`. Not used when `SP_BUILD_TRANSPORT=OFF` (including WebAssembly builds).
