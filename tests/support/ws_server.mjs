// Model-free WebSocket peer for tests/ws_transport_test.cpp: a hand-written RFC 6455 server with one
// scripted behaviour per request path, over TCP and over TLS with an ephemeral test CA.
// stdout line 1: {"port":P,"tls_port":T,"ca":"<path>"}; then one JSON reply per stdin command:
//   {"stats":true} -> what the server observed from clients (close codes, pongs, unmasked frames);
//   {"release":true} -> the sessions on /hold-reading get a Ping and are read from again.
//   {"info":true}  -> {"tls_port":T,"ca":"<path>"} (tls_port 0 when openssl is unavailable).
import net from 'node:net';
import tls from 'node:tls';
import { createHash } from 'node:crypto';
import { mkdtempSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';
import { spawnSync } from 'node:child_process';
import { tmpdir } from 'node:os';
import { createInterface } from 'node:readline';

const GUID = '258EAFA5-E914-47DA-95CA-C5AB0DC85B11';
const stats = { handshakes: 0, closes: [], pongs: [], unmasked: 0, messages: 0, realtime: [], formats: [], held: [] };
// /hold-reading reads nothing after the handshake until {"release":true}, then records the frames it receives, in
// arrival order, in stats.held (`text:<bytes>`, `binary:<bytes>`, `pong:<payload>`, `close:<code>`).
const holding = new Set();
// A realtime session in the shape of OpenAI Realtime (`/v1/realtime`, and every path under `/realtime/`): the types of
// the client events, in arrival order, are in stats.realtime, and the audio format types of a `session.update` in
// stats.formats (`input:<type>`, `output:<type>`); `response.create` answers with a turn and its usage; a text item
// whose text is `flood` answers with eight 64 KiB audio deltas, and `flood-text` with eight 64 KiB text deltas
// (`flood-text:<n>` with n of them).
// `/realtime/close/<code>/<reason>` closes the session with that code and reason after `session.created`.
// `/realtime/google` speaks the shape of Google Live instead: a `setup` is answered with `setupComplete`, and when the
// model is `tool-then-close` also with a tool call, before the session is closed.
const PCM = Buffer.alloc(1024, 3).toString('base64');
function realtimeAnswers(event) {
  if (event.type === 'response.create') return [
    { type: 'response.created', response: { id: 'resp-1', status: 'in_progress' } },
    { type: 'response.output_audio.delta', response_id: 'resp-1', delta: PCM },
    { type: 'response.done', response: { id: 'resp-1', status: 'completed', usage: { input_tokens: 10, output_tokens: 5, total_tokens: 15 } } }];
  if (event.type === 'conversation.item.create' && event.item?.content?.[0]?.text === 'flood')
    return Array.from({ length: 8 }, () => ({ type: 'response.output_audio.delta', response_id: 'resp-2', delta: Buffer.alloc(65536, 5).toString('base64') }));
  const text = event.type === 'conversation.item.create' ? event.item?.content?.[0]?.text : undefined;
  const flooding = typeof text === 'string' ? /^flood-text(?::(\d+))?$/.exec(text) : null;
  if (flooding)
    return Array.from({ length: flooding[1] === undefined ? 8 : Number(flooding[1]) }, () => ({ type: 'response.output_text.delta', response_id: 'resp-3', delta: 'x'.repeat(65536) }));
  return [];
}
const isRealtime = path => path === '/v1/realtime' || path.startsWith('/realtime/');
const sockets = new Set();
let directory;

function cleanup() {
  if (directory !== undefined) rmSync(directory, { recursive: true, force: true });
  directory = undefined;
}
process.on('exit', cleanup);

function frame(opcode, payload, { fin = true, mask = false } = {}) {
  const body = Buffer.isBuffer(payload) ? payload : Buffer.from(payload);
  const head = [((fin ? 0x80 : 0) | opcode)];
  const masked = mask ? 0x80 : 0;
  if (body.length < 126) head.push(masked | body.length);
  else if (body.length <= 0xffff) head.push(masked | 126, body.length >> 8, body.length & 0xff);
  else {
    head.push(masked | 127, 0, 0, 0, 0);
    head.push((body.length >>> 24) & 0xff, (body.length >>> 16) & 0xff, (body.length >>> 8) & 0xff, body.length & 0xff);
  }
  if (!mask) return Buffer.concat([Buffer.from(head), body]);
  const key = Buffer.from([1, 2, 3, 4]);
  const out = Buffer.from(body);
  for (let i = 0; i < out.length; ++i) out[i] ^= key[i % 4];
  return Buffer.concat([Buffer.from(head), key, out]);
}
const closeFrame = (code, reason = '') => {
  if (code === undefined) return frame(8, Buffer.alloc(0));
  const body = Buffer.alloc(2 + Buffer.byteLength(reason));
  body.writeUInt16BE(code, 0);
  body.write(reason, 2);
  return frame(8, body);
};

// Parses client frames; calls onFrame(opcode, fin, payload).
function reader(onFrame) {
  let buffered = Buffer.alloc(0);
  return chunk => {
    buffered = Buffer.concat([buffered, chunk]);
    for (;;) {
      if (buffered.length < 2) return;
      const fin = (buffered[0] & 0x80) !== 0;
      const opcode = buffered[0] & 0x0f;
      const masked = (buffered[1] & 0x80) !== 0;
      let length = buffered[1] & 0x7f;
      let offset = 2;
      if (length === 126) {
        if (buffered.length < 4) return;
        length = buffered.readUInt16BE(2);
        offset = 4;
      } else if (length === 127) {
        if (buffered.length < 10) return;
        length = Number(buffered.readBigUInt64BE(2));
        offset = 10;
      }
      const keyBytes = masked ? 4 : 0;
      if (buffered.length < offset + keyBytes + length) return;
      const key = buffered.subarray(offset, offset + keyBytes);
      const payload = Buffer.from(buffered.subarray(offset + keyBytes, offset + keyBytes + length));
      if (masked) for (let i = 0; i < payload.length; ++i) payload[i] ^= key[i % 4];
      else stats.unmasked += 1;
      buffered = buffered.subarray(offset + keyBytes + length);
      onFrame(opcode, fin, payload);
    }
  };
}

function handle(socket) {
  sockets.add(socket);
  socket.on('error', () => {});
  socket.once('close', () => sockets.delete(socket));
  let head = Buffer.alloc(0);
  const onHead = chunk => {
    head = Buffer.concat([head, chunk]);
    const end = head.indexOf('\r\n\r\n');
    if (end < 0) return;
    socket.off('data', onHead);
    const rest = head.subarray(end + 4);
    const lines = head.subarray(0, end).toString('latin1').split('\r\n');
    const target = lines[0].split(' ')[1] || '/';
    const headers = {};
    // How many times the handshake names each header the client owns.
    const owned = {};
    for (const line of lines.slice(1)) {
      const colon = line.indexOf(':');
      if (colon <= 0) continue;
      const name = line.slice(0, colon).trim().toLowerCase();
      headers[name] = line.slice(colon + 1).trim();
      if (name === 'upgrade' || name === 'connection' || name.startsWith('sec-websocket-')) owned[name] = (owned[name] || 0) + 1;
    }
    upgrade(socket, target, headers, rest, owned);
  };
  socket.on('data', onHead);
}

function upgrade(socket, target, headers, rest, owned) {
  const path = target.split('?')[0];
  if (path === '/hold-handshake') return;
  if (path === '/reject') {
    socket.end('HTTP/1.1 401 Unauthorized\r\nContent-Type: text/plain\r\nContent-Length: 6\r\nConnection: close\r\n\r\ndenied');
    return;
  }
  // /reject/<status> refuses the handshake with that status.
  const refusal = /^\/reject\/(\d{3})$/.exec(path);
  if (refusal) {
    socket.end(`HTTP/1.1 ${refusal[1]} Refused\r\nContent-Type: text/plain\r\nContent-Length: 6\r\nConnection: close\r\n\r\ndenied`);
    return;
  }
  const key = headers['sec-websocket-key'] || '';
  const accept = path === '/bad-accept' ? 'AAAAAAAAAAAAAAAAAAAAAAAAAAA=' : createHash('sha1').update(key + GUID).digest('base64');
  stats.handshakes += 1;
  let response = `HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: ${accept}\r\n\r\n`;
  const write = data => { if (!socket.destroyed) socket.write(data); };
  if (path === '/early') {
    socket.write(Buffer.concat([Buffer.from(response, 'latin1'), frame(1, 'early')]));
  } else socket.write(response);

  let sentClose = false;
  const sendClose = (code, reason) => {
    if (sentClose) return;
    sentClose = true;
    write(closeFrame(code, reason));
  };
  const onFrame = (opcode, fin, payload) => {
    if (path === '/hold-reading')
      stats.held.push(opcode === 8 ? `close:${payload.length >= 2 ? payload.readUInt16BE(0) : 0}`
        : opcode === 10 ? `pong:${payload}` : opcode === 9 ? 'ping' : `${opcode === 1 ? 'text' : 'binary'}:${payload.length}`);
    if (opcode === 8) {
      const code = payload.length >= 2 ? payload.readUInt16BE(0) : 0;
      stats.closes.push(code);
      if (path === '/ignore-close') return;
      if (!sentClose) sendClose(payload.length >= 2 ? code : undefined, '');
      socket.end();
      return;
    }
    if (opcode === 10) {
      const text = payload.toString();
      stats.pongs.push(text);
      if (path === '/ping') write(frame(1, `pong:${text}`));
      return;
    }
    if (opcode === 9) return;
    stats.messages += 1;
    if (path === '/hold-reading') return;
    if (isRealtime(path)) {
      const event = JSON.parse(payload.toString());
      stats.realtime.push(event.type ?? (event.setup ? 'setup' : 'unknown'));
      const audio = event.session?.audio;
      if (audio?.input?.format) stats.formats.push(`input:${audio.input.format.type}`);
      if (audio?.output?.format) stats.formats.push(`output:${audio.output.format.type}`);
      if (event.setup) {
        write(frame(1, JSON.stringify({ setupComplete: {} })));
        if (event.setup.model === 'models/tool-then-close') {
          write(frame(1, JSON.stringify({ toolCall: { functionCalls: [{ id: 'call-g', name: 'get_weather', args: { city: 'Seoul' } }] } })));
          sendClose(1000, 'done');
        }
      }
      for (const answer of realtimeAnswers(event)) write(frame(1, JSON.stringify(answer)));
      return;
    }
    if (path === '/fragmented') {
      write(frame(1, 'Hel', { fin: false }));
      write(frame(9, 'p'));
      write(frame(0, 'lo'));
    } else if (path === '/big') {
      write(frame(2, Buffer.alloc(2 * 1024 * 1024, 7)));
    } else if (path === '/masked') {
      write(frame(1, 'masked', { mask: true }));
    } else {
      write(frame(opcode, payload));
    }
  };
  const feed = reader(onFrame);
  socket.on('data', feed);
  if (rest.length) feed(rest);
  if (path === '/hold-reading') {
    socket.pause();
    holding.add(socket);
    socket.once('close', () => holding.delete(socket));
  }

  const closing = /^\/realtime\/close\/(\d+)\/(.*)$/.exec(path);
  if (isRealtime(path)) {
    if (!path.startsWith('/realtime/google')) write(frame(1, JSON.stringify({ type: 'session.created', session: { id: 'sess-local' } })));
    if (closing) sendClose(Number(closing[1]), decodeURIComponent(closing[2]));
  } else if (path === '/headers') {
    write(frame(1, JSON.stringify({ target, authorization: headers.authorization || '', test: headers['x-test'] || '', host: headers.host || '', owned })));
  } else if (path === '/ping') {
    write(frame(9, 'probe'));
  } else if (path === '/server-close') {
    sendClose(4001, 'bye');
  } else if (path === '/server-close-nostatus') {
    sendClose(undefined, '');
  } else if (path === '/drop') {
    write(frame(1, 'bye'));
    setTimeout(() => socket.destroy(), 50);
  }
}

function openssl(...args) {
  const result = spawnSync(process.env.SP_OPENSSL || 'openssl', args, { cwd: directory, stdio: 'ignore', timeout: 10_000 });
  if (result.error || result.status !== 0) throw new Error(`openssl ${args[0]} failed`);
}

async function listen(server) {
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  return server.address().port;
}

async function start() {
  const plain = net.createServer(handle);
  plain.on('error', () => process.exit(1));
  const port = await listen(plain);
  let tlsPort = 0;
  let ca = '';
  try {
    // TLS is skipped (tls_port 0) where openssl is unavailable; the TCP cases still run.
    directory = mkdtempSync(join(tmpdir(), 'schemaprovider-ws-'));
    writeFileSync(join(directory, 'leaf.ext'),
      'basicConstraints=critical,CA:FALSE\nkeyUsage=critical,digitalSignature,keyEncipherment\n' +
      'extendedKeyUsage=serverAuth\nsubjectAltName=DNS:localhost,IP:127.0.0.1\n', { mode: 0o600 });
    openssl('req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-sha256', '-keyout', 'ca.key', '-out', 'ca.pem',
      '-days', '2', '-subj', '/CN=SchemaProvider ephemeral WebSocket CA',
      '-addext', 'basicConstraints=critical,CA:TRUE', '-addext', 'keyUsage=critical,keyCertSign,cRLSign');
    openssl('req', '-new', '-newkey', 'rsa:2048', '-nodes', '-sha256', '-keyout', 'leaf.key', '-out', 'leaf.csr',
      '-subj', '/CN=localhost');
    openssl('x509', '-req', '-in', 'leaf.csr', '-CA', 'ca.pem', '-CAkey', 'ca.key', '-set_serial', '1',
      '-out', 'leaf.pem', '-days', '2', '-sha256', '-extfile', 'leaf.ext');
    const secure = tls.createServer({
      key: readFileSync(join(directory, 'leaf.key')),
      cert: readFileSync(join(directory, 'leaf.pem')),
      ALPNProtocols: ['http/1.1'],
    }, handle);
    secure.on('tlsClientError', () => {});
    tlsPort = await listen(secure);
    ca = join(directory, 'ca.pem');
  } catch {
    tlsPort = 0;
  }
  process.stdout.write(JSON.stringify({ port, tls_port: tlsPort, ca }) + '\n');
  const input = createInterface({ input: process.stdin });
  input.on('line', line => {
    let command;
    try { command = JSON.parse(line); } catch { process.stdout.write('{"error":"bad command"}\n'); return; }
    if (command.stats) process.stdout.write(JSON.stringify(stats) + '\n');
    else if (command.info) process.stdout.write(JSON.stringify({ tls_port: tlsPort, ca }) + '\n');
    else if (command.release) {
      const released = holding.size;
      for (const socket of holding) {
        // The Ping is answered behind whatever the client still has queued.
        if (!socket.destroyed) socket.write(frame(9, 'held'));
        socket.resume();
      }
      holding.clear();
      process.stdout.write(JSON.stringify({ released }) + '\n');
    }
    else process.stdout.write('{"error":"unknown command"}\n');
  });
  input.on('close', () => {
    for (const socket of sockets) socket.destroy();
    process.exit(0);
  });
}

start().catch(() => process.exit(1));
