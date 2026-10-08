// Model-free stall oracle for the transport stall bounds (connect, first byte, idle). It serves the
// scripted misbehaviour of a peer that goes quiet at a chosen point, over HTTP/1.1, cleartext HTTP/2
// (prior knowledge), TLS (ALPN h2 or http/1.1) and a TCP listener that accepts and never speaks
// (the TLS handshake black hole). Control uses the same JSON-lines protocol as runtime_peer.h:
//   {"arm":"<name>","scenario":"<scenario>[:p1[:p2]]"}            register a case
//   {"model":"<name>"}                                           counters of the case
//   {"model":"<name>","wait":{"count":1}}                        block until counters reach minimums
//   {"model":"<name>","release":true}                            end held responses
//   {"ports":true}                                               listener ports and the CA file
// A case is addressed by path (/t/<name>) or, for the five provider families, by the model name in the
// request body (Gemini: in the path). Replies contain counters only, never request content.
//
// Scenarios (names; p1, p2 are integers, milliseconds unless noted):
//   ok                 complete, valid response at once (families: chat and messages only)
//   none               read the whole request, never answer
//   head-silent        200 + SSE headers, then silence
//   prefix-silent      200 + SSE headers + the family's valid first events, then silence
//   trickle:p1         prefix, then one valid delta event every p1 ms until the client goes away
//   comments:p1        200 + SSE headers, then an SSE comment every p1 ms (no events)
//   late-head:p1       wait p1 ms, then the complete response
//   slow-body:p1       200 + content-length 40 headers, then one body byte every p1 ms
//   slow-head:p1       status line, then eight header lines every p1 ms, then a silent SSE body (H1)
//   slow-read:p1:p2    read the request body p1 bytes per p2 ms, answer when it has all arrived
//   reset              abort the connection (RST) after the request arrived
import http from 'node:http';
import http2 from 'node:http2';
import net from 'node:net';
import readline from 'node:readline';
import { spawnSync } from 'node:child_process';
import { mkdtempSync, readFileSync, rmSync } from 'node:fs';
import { join } from 'node:path';

const cases = new Map();
const sockets = new Set();
const waiters = new Set();
const timers = new Set();
let unexpected = 0;
let handshakeConnections = 0;
let directory;
const reply = value => process.stdout.write(`${JSON.stringify(value)}\n`);
const notify = () => { for (const wake of [...waiters]) wake(); };
// Connection identity: two requests with the same `connection` shared one HTTP/2 session or TCP socket.
const identities = new WeakMap();
let nextIdentity = 0;
const identify_connection = req => {
  const owner = req.stream?.session ?? req.socket;
  if (!identities.has(owner)) identities.set(owner, ++nextIdentity);
  return identities.get(owner);
};
const snapshot = c => ({ count: c.count, faults: c.faults, invalid: c.invalid, unexpected,
  closed: c.closed, ended: c.ended, upload_bytes: c.upload_bytes, handshake: handshakeConnections,
  connection: c.connection ?? 0 });

const frame = (type, value, named) => `${named ? `event: ${type}\n` : ''}data: ${JSON.stringify(value)}\n\n`;
const chatChunk = (model, content) => frame('', { id: 'chat-stall', object: 'chat.completion.chunk', created: 1, model,
  choices: [{ index: 0, delta: { role: 'assistant', content }, finish_reason: null }] }, false);
const geminiChunk = (model, text) => frame('', { modelVersion: model, responseId: 'stall',
  candidates: [{ index: 0, content: { role: 'model', parts: [{ text }] } }] }, false);
const typed = (type, fields) => `event: ${type}\ndata: ${JSON.stringify({ type, ...fields })}\n\n`;
const interaction = (event_type, fields) => `event: ${event_type}\ndata: ${JSON.stringify({ event_type, ...fields })}\n\n`;
const responseObject = (model, status) => ({ id: 'resp_stall', object: 'response', created_at: 1, model, status,
  output: [], usage: null, incomplete_details: null, error: null });

// The valid opening events of each family and one repeatable delta.
function prefix(family, model) {
  switch (family) {
    case 'chat': return chatChunk(model, 'partial');
    case 'messages': return frame('message_start', { type: 'message_start', message: { id: 'message-stall', type: 'message',
      role: 'assistant', model, content: [], stop_reason: null, stop_sequence: null, usage: { input_tokens: 2, output_tokens: 0 } } }, true)
      + frame('content_block_start', { type: 'content_block_start', index: 0, content_block: { type: 'text', text: '' } }, true)
      + frame('content_block_delta', { type: 'content_block_delta', index: 0, delta: { type: 'text_delta', text: 'partial' } }, true);
    case 'responses': return typed('response.created', { response: responseObject(model, 'in_progress') })
      + typed('response.output_item.added', { output_index: 0, item: { id: 'msg_stall', type: 'message', role: 'assistant', status: 'in_progress', content: [] } })
      + typed('response.content_part.added', { item_id: 'msg_stall', output_index: 0, content_index: 0, part: { type: 'output_text', text: '', annotations: [] } })
      + typed('response.output_text.delta', { item_id: 'msg_stall', output_index: 0, content_index: 0, delta: 'partial' });
    case 'gemini': return geminiChunk(model, 'partial');
    case 'interactions': return interaction('interaction.created', { interaction: { id: 'interaction_stall', model, status: 'in_progress' } })
      + interaction('step.start', { index: 0, step: { type: 'model_output', content: [] } })
      + interaction('step.delta', { index: 0, delta: { type: 'text', text: 'partial' } });
    default: return 'data: {"delta":"x"}\n\n';
  }
}
function delta(family, model) {
  switch (family) {
    case 'chat': return chatChunk(model, 'x');
    case 'messages': return frame('content_block_delta', { type: 'content_block_delta', index: 0, delta: { type: 'text_delta', text: 'x' } }, true);
    case 'responses': return typed('response.output_text.delta', { item_id: 'msg_stall', output_index: 0, content_index: 0, delta: 'x' });
    case 'gemini': return geminiChunk(model, 'x');
    case 'interactions': return interaction('step.delta', { index: 0, delta: { type: 'text', text: 'x' } });
    default: return 'data: {"delta":"x"}\n\n';
  }
}
// A complete, valid answer for the families whose normal path the tests exercise.
function complete(family, model, streaming) {
  if (family === 'chat') {
    if (!streaming) return JSON.stringify({ id: 'chat-stall', object: 'chat.completion', created: 1, model,
      choices: [{ index: 0, message: { role: 'assistant', content: 'hello' }, finish_reason: 'stop' }],
      usage: { prompt_tokens: 2, completion_tokens: 3, total_tokens: 5 } });
    return chatChunk(model, 'hello') + frame('', { id: 'chat-stall', object: 'chat.completion.chunk', created: 1, model,
      choices: [{ index: 0, delta: {}, finish_reason: 'stop' }], usage: { prompt_tokens: 2, completion_tokens: 3, total_tokens: 5 } }, false)
      + 'data: [DONE]\n\n';
  }
  if (family === 'messages') {
    if (!streaming) return JSON.stringify({ id: 'message-stall', type: 'message', role: 'assistant', model,
      content: [{ type: 'text', text: 'hello' }], stop_reason: 'end_turn', stop_sequence: null, usage: { input_tokens: 2, output_tokens: 3 } });
    return prefix('messages', model).replace('partial', 'hello')
      + frame('content_block_stop', { type: 'content_block_stop', index: 0 }, true)
      + frame('message_delta', { type: 'message_delta', delta: { stop_reason: 'end_turn', stop_sequence: null }, usage: { output_tokens: 3 } }, true)
      + frame('message_stop', { type: 'message_stop' }, true);
  }
  return null;
}

function parseScenario(text) {
  const [name, ...rest] = String(text).split(':');
  return { name, p1: Number(rest[0] ?? 0), p2: Number(rest[1] ?? 0) };
}
function pace(ms, fn) {
  const timer = setInterval(fn, ms);
  timers.add(timer);
  return () => { clearInterval(timer); timers.delete(timer); };
}
function later(ms, fn) {
  const timer = setTimeout(() => { timers.delete(timer); fn(); }, ms);
  timers.add(timer);
  return () => { clearTimeout(timer); timers.delete(timer); };
}

function identify(req, body) {
  const url = req.url;
  let m;
  if ((m = /^\/t\/([A-Za-z0-9_-]+)/.exec(url))) return { family: 'plain', id: m[1], model: m[1], streaming: true };
  if (url === '/v1/chat/completions') return { family: 'chat', id: body?.model, model: body?.model, streaming: body?.stream === true };
  if (url === '/v1/messages') return { family: 'messages', id: body?.model, model: body?.model, streaming: body?.stream === true };
  if (url === '/v1/responses') return { family: 'responses', id: body?.model, model: body?.model, streaming: body?.stream === true };
  if (url === '/v1beta/interactions') return { family: 'interactions', id: body?.model, model: body?.model, streaming: body?.stream === true };
  if ((m = /^\/v1beta\/models\/([^/:]+):(streamGenerateContent|generateContent)/.exec(url)))
    return { family: 'gemini', id: m[1], model: m[1], streaming: m[2] === 'streamGenerateContent' };
  return null;
}

function handle(req, res) {
  const cancels = [];
  let c;
  let target;
  const finishScenario = () => {
    const sc = c.scenario;
    const info = target;
    const sse = info.streaming;
    const head = () => {
      res.writeHead(200, { 'Content-Type': sse ? 'text/event-stream' : 'application/json' });
      res.flushHeaders();
    };
    const send = text => { if (!res.destroyed && !res.writableEnded) res.write(text); };
    const respond = () => {
      if (res.destroyed) return;
      if (info.family === 'plain') {
        res.writeHead(200, { 'Content-Type': 'text/plain', 'Content-Length': '2' });
        res.end('ok');
        return;
      }
      const full = complete(info.family, info.model, sse);
      if (full === null) { ++c.invalid; res.writeHead(500); res.end(); return; }
      res.writeHead(200, { 'Content-Type': sse ? 'text/event-stream' : 'application/json' });
      res.end(full);
    };
    switch (sc.name) {
      case 'none': return;
      case 'reset':
        if (res.socket && res.socket.resetAndDestroy) res.socket.resetAndDestroy(); else res.destroy();
        return;
      case 'head-silent': head(); return;
      case 'prefix-silent': head(); send(prefix(info.family, info.model)); return;
      case 'slow-head': {
        // Use raw H1 response-head bytes so each header line moves independently of the body.
        const socket = res.socket;
        if (!socket || req.httpVersionMajor !== 1) { ++c.invalid; res.destroy(); return; }
        socket.write('HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nTransfer-Encoding: chunked\r\n');
        let sent = 0;
        const stop = pace(sc.p1, () => {
          if (socket.destroyed) { stop(); return; }
          socket.write(`X-Activity-${++sent}: yes\r\n`);
          if (sent === 8) { stop(); socket.write('\r\n'); }
        });
        cancels.push(stop);
        return;
      }
      case 'trickle':
        head(); send(prefix(info.family, info.model));
        cancels.push(pace(sc.p1, () => send(delta(info.family, info.model))));
        return;
      case 'comments':
        head(); send(': start\n\n');
        cancels.push(pace(sc.p1, () => send(': keepalive\n\n')));
        return;
      case 'slow-body': {
        res.writeHead(200, { 'Content-Type': 'application/octet-stream', 'Content-Length': '40' });
        res.flushHeaders();
        let sent = 0;
        const stop = pace(sc.p1, () => {
          if (res.destroyed) { stop(); return; }
          res.write('x');
          if (++sent === 40) { stop(); res.end(); }
        });
        cancels.push(stop);
        return;
      }
      case 'late-head': cancels.push(later(sc.p1, respond)); return;
      case 'ok': case 'slow-read': respond(); return;
      default: ++c.invalid;
    }
  };
  const run = () => {
    ++c.ended;
    try { finishScenario(); } catch { ++c.invalid; res.destroy(); }
    notify();
  };
  const resolve = body => {
    target = identify(req, body);
    c = target && cases.get(target.id);
    if (!c) { ++unexpected; res.writeHead(400); res.end(); return false; }
    ++c.count;
    c.connection = identify_connection(req);
    notify();
    return true;
  };
  res.on('close', () => {
    if (c && !res.writableFinished) ++c.closed;
    cancels.splice(0).forEach(stop => stop());
    notify();
  });
  res.on('error', () => {});
  req.on('error', () => {});

  if (req.url.startsWith('/t/')) {
    // Transport cases are known from the path, so the body can be paced from the first byte.
    if (!resolve(null)) { req.resume(); return; }
    if (c.scenario.name === 'slow-read') {
      const { p1, p2 } = c.scenario;
      req.on('readable', () => {});
      const stop = pace(p2, () => { const piece = req.read(p1); if (piece) c.upload_bytes += piece.length; });
      cancels.push(stop);
      req.on('end', () => { stop(); run(); });
    } else {
      req.on('data', chunk => { c.upload_bytes += chunk.length; });
      req.on('end', run);
    }
    return;
  }
  // Family requests: the case is identified by the model in the JSON body (Gemini: the path).
  let text = '';
  req.on('data', chunk => { text += chunk.toString('utf8'); if (text.length > (1 << 20)) req.destroy(); });
  req.on('end', () => {
    let body = null;
    try { body = JSON.parse(text); } catch { body = null; }
    if (!resolve(body)) return;
    c.upload_bytes += Buffer.byteLength(text);
    run();
  });
}

function attach(server) {
  server.on('connection', socket => { sockets.add(socket); socket.on('error', () => {}); socket.on('close', () => sockets.delete(socket)); });
  server.on('secureConnection', socket => { sockets.add(socket); socket.on('error', () => {}); socket.on('close', () => sockets.delete(socket)); });
  server.on('clientError', (_e, socket) => socket.destroy());
  server.on('tlsClientError', (_e, socket) => socket.destroy());
  server.on('session', session => { session.on('error', () => {}); sockets.add(session); session.on('close', () => sockets.delete(session)); });
  server.on('stream', stream => stream.on('error', () => {}));
  return server;
}
const listen = server => new Promise(resolve => server.listen(0, '127.0.0.1', () => resolve(server.address().port)));

function certificate() {
  directory = mkdtempSync('/tmp/schemaprovider-stall-');
  const result = spawnSync('openssl', ['req', '-x509', '-newkey', 'ec', '-pkeyopt', 'ec_paramgen_curve:prime256v1',
    '-nodes', '-sha256', '-keyout', 'key.pem', '-out', 'cert.pem', '-days', '2', '-subj', '/CN=localhost',
    '-addext', 'basicConstraints=critical,CA:TRUE', '-addext', 'keyUsage=critical,digitalSignature,keyCertSign',
    '-addext', 'subjectAltName=DNS:localhost,IP:127.0.0.1'],
  { cwd: directory, stdio: ['ignore', 'ignore', 'ignore'], timeout: 15000 });
  if (result.error || result.status !== 0) throw new Error('openssl failed');
  return { key: readFileSync(join(directory, 'key.pem')), cert: readFileSync(join(directory, 'cert.pem')),
    ca: join(directory, 'cert.pem') };
}

function shutdown() {
  for (const timer of timers) { clearInterval(timer); clearTimeout(timer); }
  for (const socket of sockets) socket.destroy();
  if (directory) rmSync(directory, { recursive: true, force: true });
  process.exit(0);
}
process.on('SIGTERM', shutdown);
process.on('SIGINT', shutdown);

const material = certificate();
const h1 = attach(http.createServer(handle));
const h2c = attach(http2.createServer({}, handle));
const tls = attach(http2.createSecureServer({ key: material.key, cert: material.cert, allowHTTP1: true }, handle));
const blackhole = net.createServer(socket => { ++handshakeConnections; sockets.add(socket); socket.on('error', () => {});
  socket.on('close', () => sockets.delete(socket)); notify(); });
const ports = { port: await listen(h1), h2c: await listen(h2c), tls: await listen(tls), handshake: await listen(blackhole),
  ca: material.ca };
reply(ports);

const control = readline.createInterface({ input: process.stdin });
control.on('line', async line => {
  try {
    const command = JSON.parse(line);
    if (command.ports) { reply(ports); return; }
    if (command.arm) {
      if (cases.has(command.arm)) throw new Error('duplicate case');
      cases.set(command.arm, { scenario: parseScenario(command.scenario), count: 0, faults: 0, invalid: 0, closed: 0,
        ended: 0, upload_bytes: 0 });
      reply({ armed: true }); return;
    }
    const c = cases.get(command.model);
    if (!c) throw new Error('unknown case');
    if (command.wait) await new Promise((resolve, reject) => {
      const ready = () => {
        const s = snapshot(c);
        if (Object.entries(command.wait).every(([key, minimum]) => s[key] >= minimum)) {
          clearTimeout(timer); waiters.delete(ready); resolve();
        }
      };
      const timer = setTimeout(() => { waiters.delete(ready); reject(new Error('peer wait timeout')); }, 12000);
      waiters.add(ready); ready();
    });
    reply(snapshot(c));
  } catch { reply({ error: 'stall peer control failure' }); }
});
control.on('close', shutdown);
