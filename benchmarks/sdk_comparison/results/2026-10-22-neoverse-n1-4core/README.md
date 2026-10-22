# Session 2026-10-22, Neoverse-N1, 4 cores

The first full run of the SDK comparison. `tables.md` is generated from the records in this directory by `render.py` (`python3 render.py <this directory> > tables.md`), and `tests/test_results.py` fails if the file differs from that output. This page describes the session; it quotes no measured number, so every number is in `tables.md` or in the records.

## What was run

- One session, one machine: an Arm server with four Neoverse-N1 cores, Ubuntu 24.04, no frequency scaling interface. The peer and its two serving processes were pinned to CPUs 0 and 1, the client under test to CPUs 2 and 3, for every contender. The load average before and after is in `load-before.txt` and `load-after.txt`; no other benchmark process was running.
- `sdkbench.py all --reps 3 --cold-reps 20 --reruns 1 --peer-cpus 0,1 --peer-workers 2 --client-cpus 2,3`, from the harness commit named in `session.json` (the working tree was clean).
- Both APIs (OpenAI Chat Completions, Anthropic Messages), both modes (buffered, streamed), concurrency 1, 8, 32 and 128, three fresh client processes per cell, 1,000 or more measured requests per cell (20 per in-flight slot above 50), HTTP/1.1, pool size equal to the concurrency where the SDK allows it.
- SchemaProvider and the null client ran twice, against libcurl 8.5.0 (the distribution's) and against libcurl 8.23.0 built from the official tarball into a private prefix. Rows carry the libcurl version in their label. `tables.md` lists, for each row, the version string the program printed at run time, its TLS library, its HTTP/2 library and the features of the build.

## What differs between the two SchemaProvider rows

Same SchemaProvider tree, compiler, flags, CA file, options, peer and CPU pinning. They differ in libcurl itself (8.5.0 against 8.23.0), in the nghttp2 beneath it (1.59.0 against 1.70.0; the private build brings its own because the host has no development files for it), and in what libcurl was configured with: the distribution's build links compression, PSL, IDN, LDAP, SSH and RTMP libraries and the private build does not, which neither client uses but which changes the number of shared libraries a process loads. The TLS library, OpenSSL 3.0.13, is the same in both. A difference between the two rows is therefore a difference between two builds, not proof of an effect of the libcurl version alone.

## Things to know before reading the tables

- **Closed loop.** The numbers are service times at fixed concurrency, not response times under an arrival rate.
- **The peer saturates.** The mock peer is a Node process with two serving processes on two cores. Cells in which it used at least 80% of the cores it could use are marked ‡; for the fastest clients most cells at concurrency 8 and above are marked, and in those cells throughput and latency measure the peer's ceiling as much as the client. Client CPU per request, peak memory and the cells without ‡ are not affected in the same way. Peer CPU is recorded and never subtracted.
- **Python and Node clients use one core.** The two cores given to the client can be used by a multi-threaded native client and by a Python or Node process only in part, so the comparison between those groups mixes language runtime with SDK design; the single-provider SDK rows are the baseline for the Python group.
- **Connection reuse.** The runner requires zero new connections in the measured window. SchemaProvider and the null client met that in every cell. Rows marked † did not, for the reason printed under each table: ai-sdk-cpp opens a connection per request, and genai, aisuite, any-llm and the Vercel AI SDK opened new connections in some cells. The null client with a fresh connection per request is the floor for the first case.
- **Annotation made after the run.** The runner rejected the streamed cells of the Vercel AI SDK that had new connections, twice each (one rerun). Rather than leave the SDK without streamed results, `annotations.json` tells the renderer to show those cells as valid with a † flag, as for the other clients whose pool the runner cannot hold steady. The records themselves are unchanged and still say `invalid`. This choice was made after seeing the data.
- **n/a.** ai-sdk-cpp has no buffered cells (its buffered call cannot reach a TLS server on a port other than 443), and each of the two single-provider Python SDKs has one API. The reasons are in `tables.md`.
- **Memory.** Peak resident size includes the interpreter or runtime. The marginal figure per in-flight request is the difference between two separate processes divided by the difference in concurrency; for managed runtimes whose heap grows in steps (Node, Python) it is only an upper-level indication.
- **Install footprint** counts stripped binaries without their shared libraries, and, for Python and Node, the whole environment.
- **Cold start** is the wall time from spawning the process to its first verified request against a running peer, 20 processes after one uncounted warm spawn, with the clients' files already in the page cache.

## Files

| file | content |
|---|---|
| `session.json` | provenance: harness commit and dirty flag, CPU model and count, kernel, tool versions, UTC time, hashes of the runner, renderer, workload, manifest and peer, and the installed record of every contender (commit or release, artifact hash, libcurl build facts) |
| `cells.jsonl` | one record per attempt of every cell (including invalid attempts and n/a cells) |
| `cold.jsonl`, `sizes.jsonl` | cold-start and footprint records |
| `samples/` | raw per-request latencies of every cell, gzip-compressed JSON |
| `logs/` | stderr of every client process |
| `runner.log` | the runner's console output |
| `annotations.json` | the post-run annotation above and the comments printed with the tables |
| `load-before.txt`, `load-after.txt` | load average and the busiest processes before and after the session |
| `tables.md` | the tables, generated |

The scratch directories of the runner (peer configurations and an empty home directory) are not kept.
