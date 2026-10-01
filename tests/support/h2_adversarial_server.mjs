// Model-free HTTP/2 peers for multiplexing, retry, and resolver tests.
// /ok responds with "peer A\n" or "peer B\n"; keyed stats identify its session/stream.
// /flood writes 'z' bytes, stopping immediately when write() reports backpressure.
// Per-key written counts bytes accepted by write/end; done counts writable finish;
// closed counts stream close. Session/stream record the most recent keyed request.
// GET /close on the stats listener closes only sessions with zero live streams.
// Active sessions are untouched; idle sockets are closed without waiting for a client GOAWAY.
import http from 'node:http';
import http2 from 'node:http2';

const { NGHTTP2_NO_ERROR, NGHTTP2_REFUSED_STREAM } = http2.constants;
const block = Buffer.alloc(64 * 1024, 'z');
const counters = new Map([
  ['sessions', 0],
  ['active_sessions', 0],
  ['peer_A_requests', 0],
  ['peer_B_requests', 0],
  ['total_requests', 0],
  ['retired_sessions', 0],
]);
const sessions = new Map();
const sockets = new Set();
const firstGoaway = new Set();
const firstRefused = new Set();
let nextSession = 0;
let stopping = false;

function increment(name, amount = 1) {
  counters.set(name, (counters.get(name) ?? 0) + amount);
}

function safeStreamAction(stream, action) {
  if (stream.destroyed || stream.closed) return;
  try {
    action();
  } catch {
    stream.destroy();
  }
}

function respond(stream, status, body, key) {
  safeStreamAction(stream, () => {
    stream.respond({ ':status': status, 'content-length': String(Buffer.byteLength(body)) });
    stream.end(body);
    if (key !== null) increment(`${key}_written`, Buffer.byteLength(body));
  });
}

function trackSocket(socket) {
  sockets.add(socket);
  socket.on('error', () => {});
  socket.on('close', () => sockets.delete(socket));
}

function createPeer(peer) {
  const server = http2.createServer();
  server.on('connection', trackSocket);
  server.on('session', (session) => {
    const state = { id: ++nextSession, active: 0, retiring: false };
    sessions.set(session, state);
    increment('sessions');
    increment('active_sessions');
    session.on('error', () => {});
    session.on('close', () => {
      sessions.delete(session);
      increment('active_sessions', -1);
    });
  });
  server.on('sessionError', () => {});
  server.on('stream', (stream, headers) => {
    stream.on('error', () => {});
    const state = sessions.get(stream.session);
    if (!state) {
      stream.destroy();
      return;
    }
    state.active += 1;
    increment('total_requests');
    increment(`peer_${peer}_requests`);
    let key = null;
    stream.on('close', () => {
      state.active -= 1;
      if (key !== null) increment(`${key}_closed`);
    });
    stream.on('finish', () => {
      if (key !== null) increment(`${key}_done`);
    });

    let url;
    try {
      url = new URL(headers[':path'], 'http://fixture.invalid');
    } catch {
      stream.resume();
      respond(stream, 400, 'invalid-path\n', null);
      return;
    }
    const requestedKey = url.searchParams.get('key');
    if (requestedKey !== null && !/^[A-Za-z0-9]{1,32}$/.test(requestedKey)) {
      stream.resume();
      respond(stream, 400, 'invalid-key\n', null);
      return;
    }
    key = requestedKey;
    if (key !== null) {
      for (const suffix of ['requests', 'written', 'done', 'closed', 'session', 'stream']) {
        const name = `${key}_${suffix}`;
        if (!counters.has(name)) counters.set(name, 0);
      }
      increment(`${key}_requests`);
      counters.set(`${key}_session`, state.id);
      counters.set(`${key}_stream`, stream.id);
    }

    if (url.pathname === '/ok') {
      // Drain and accept the entire upload before replying; never retain its contents.
      stream.on('end', () => respond(
        stream, 200, `peer ${peer}\n`, key,
      ));
      stream.resume();
      return;
    }
    stream.resume();
    if (url.pathname === '/flood') {
      const rawTotal = url.searchParams.get('total');
      const total = Number(rawTotal);
      if (key === null || rawTotal === null || !/^[0-9]+$/.test(rawTotal)
          || !Number.isSafeInteger(total) || total < 0) {
        respond(stream, 400, 'invalid-flood-parameters\n', key);
        return;
      }
      safeStreamAction(stream, () => {
        stream.respond({ ':status': 200, 'content-length': String(total) });
        let sent = 0;
        const pump = () => safeStreamAction(stream, () => {
          while (sent < total) {
            const size = Math.min(block.length, total - sent);
            const writable = stream.write(size === block.length ? block : block.subarray(0, size));
            sent += size;
            increment(`${key}_written`, size);
            if (!writable) {
              stream.once('drain', pump);
              return;
            }
          }
          stream.end();
        });
        pump();
      });
      return;
    }
    if (url.pathname === '/goaway' || url.pathname === '/refused') {
      if (key === null) {
        respond(stream, 400, 'missing-key\n', null);
        return;
      }
      // Node treats lastStreamID=0 as "latest processed stream", not wire zero. Require a
      // warm-up stream so a positive lower ID can truthfully mark this request unprocessed.
      if (url.pathname === '/goaway' && stream.id === 1) {
        respond(stream, 400, 'warm-connection-first\n', key);
        return;
      }
      const seen = url.pathname === '/goaway' ? firstGoaway : firstRefused;
      if (seen.has(key)) {
        respond(stream, 200, 'unexpected-resend', key);
      } else {
        seen.add(key);
        safeStreamAction(stream, () => {
          if (url.pathname === '/goaway') {
            stream.session.goaway(NGHTTP2_NO_ERROR, stream.id - 2);
          } else {
            stream.close(NGHTTP2_REFUSED_STREAM);
          }
        });
      }
      return;
    }
    respond(stream, 404, 'not-found\n', key);
  });
  return server;
}

const peerA = createPeer('A');
const peerB = createPeer('B');
const stats = http.createServer((request, response) => {
  request.on('error', () => {});
  response.on('error', () => {});
  request.resume();
  response.setHeader('connection', 'close');
  response.setHeader('content-type', 'text/plain');
  if (request.method === 'GET' && request.url === '/close') {
    let retired = 0;
    for (const [session, state] of sessions) {
      if (state.active !== 0 || state.retiring || session.destroyed || session.closed) continue;
      state.retiring = true;
      // destroy() sends GOAWAY and closes this idle socket without waiting for the client's
      // reciprocal close. A connection cache need not monitor sockets with no active handle.
      try {
        session.destroy();
        increment('retired_sessions');
        retired += 1;
      } catch {
        // A concurrent peer close requires no further action.
      }
    }
    response.end(`retired ${retired}\n`);
    return;
  }
  response.end(Array.from(counters, ([name, value]) => `${name} ${value}\n`).join(''));
});
stats.on('connection', trackSocket);
stats.on('clientError', (_error, socket) => socket.destroy());

function shutdown(code = 0) {
  if (stopping) return;
  stopping = true;
  for (const session of sessions.keys()) session.destroy();
  for (const socket of sockets) socket.destroy();
  for (const server of [peerA, peerB, stats]) server.close();
  // Explicit exit also bounds shutdown when HTTP/2 flow control holds a stream open.
  process.exit(code);
}

for (const server of [peerA, peerB, stats]) {
  server.on('error', (error) => {
    console.error(`HTTP fixture listener failure: ${error.code ?? 'unknown'}`);
    shutdown(1);
  });
}
process.stdin.on('error', () => shutdown(1));
process.stdin.on('end', () => shutdown());
process.stdin.resume();
process.on('SIGTERM', () => shutdown());
process.on('SIGINT', () => shutdown());

peerA.listen(0, '127.0.0.1', () => {
  const port = peerA.address().port;
  peerB.listen(port, '127.0.0.2', () => {
    stats.listen(0, '127.0.0.1', () => {
      console.log(`PORTS h2=${port} stats=${stats.address().port}`);
    });
  });
});
