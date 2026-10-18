# Autobahn|Testsuite conformance of the WebSocket client

The [Autobahn|Testsuite](https://github.com/crossbario/autobahn-testsuite) fuzzing server is the common conformance oracle for RFC 6455 implementations. Here it runs against `sp_ws_client_tool`, a driver over `open_websocket` (`src/transport/ws_transport.h`). The driver connects to each case and echoes every message the server sends, which is what the suite expects of a client.

## Running it

```sh
tests/autobahn/run.sh <build-directory>
```

The script needs Docker and the official image `crossbario/autobahn-testsuite`. It starts the fuzzing server on `ws://127.0.0.1:9001` with `fuzzingserver.json`, runs every case through the driver, and prints the summary that `summarize.py` writes. It exits non-zero unless every case ends `OK`, `NON-STRICT` or `INFORMATIONAL` with a clean close. The run takes about a minute. It is not part of CTest, because it needs Docker.

## Scope

- Cases 1 to 10 run: framing, pings and pongs, reserved bits, opcodes, fragmentation, UTF-8 handling, limits and the closing handshake.
- Cases 12 and 13 (`permessage-deflate`) are excluded. The client offers no extension, and it refuses a server that selects one.

## Results

`results.json` holds the summary of the last recorded run.
- **Environment:** Linux x86_64, libcurl 8.5.0, the image's Autobahn 0.10.9 and AutobahnTestSuite 25.10.1.
- **Verdicts:**
  - 301 cases;
  - 298 `OK`;
  - 3 `INFORMATIONAL`: 7.1.6, 7.13.1 and 7.13.2, which record how the close was observed and carry no pass/fail;
  - no `NON-STRICT` and no `FAILED`;
  - every close behaviour `OK` or `INFORMATIONAL`.
