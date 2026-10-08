// Model-free lifecycle oracle for the family x lifecycle matrix (tests/matrix_lifecycle_test.cpp).
//
// One process serves the five wire formats (Chat Completions, Responses, Messages, Gemini
// generateContent, Interactions) on four listeners: HTTP/1.1, cleartext HTTP/2 (prior knowledge),
// and HTTPS with ALPN h2 + http/1.1 (self-signed, ephemeral, loopback-only), plus a raw TCP
// "black hole" that accepts connections and never reads or writes. The C++ side arms a model name
// with a plan: one action per request ordinal (the last action repeats). Control replies carry
// counters only, never request headers or bodies.
//
// Control (one JSON object per line on stdin, one reply per line on stdout):
//   {"arm":<model>,"plan":[{"a":<action>,...},...]}   -> {"armed":true}
//   {"info":true}                                     -> listener ports and the CA file
//   {"model":<m>,"wait":{<counter>:<min>}}            -> blocks (<=12 s) until counters reach minima
//   {"model":<m>,"release":true}                      -> closes every held response
//   {"model":<m>}                                     -> counters
// Actions: ok, status, stall-head, stall-after-head, stall-mid, trickle, close-after-head, reset.
import http from 'node:http';
import http2 from 'node:http2';
import net from 'node:net';
import readline from 'node:readline';
import { spawnSync } from 'node:child_process';
import { mkdtempSync, readFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

const key = 'MATRIX_SECRET_MARKER_5d1e90';
const cases = new Map(), waiters = new Set(), sockets = new Set(), ids = new WeakMap();
const tcpSockets = new Map();
const tcpKey = socket => `${socket.localPort}:${socket.remotePort}`;
let unexpected = 0, nextId = 0, blackholeConnections = 0, directory;
const reply = value => process.stdout.write(`${JSON.stringify(value)}\n`);
const notify = () => { for (const wake of [...waiters]) wake(); };
const now = () => Number(process.hrtime.bigint()) / 1e6;
const snapshot = c => ({ count: c.count, times: c.times, held: c.held.size, closed: c.closed, faults: c.faults,
  invalid: c.invalid, unexpected, bytes: c.bytes, h2: c.h2, tls: c.tls, conns: c.conns.size, reused: c.reused,
  blackhole: blackholeConnections });

// ---- wire shapes -------------------------------------------------------------------------------
const usageGoogle = { promptTokenCount: 10, candidatesTokenCount: 7, totalTokenCount: 17 };
const sse = (type, value) => `${type ? `event: ${type}\n` : ''}data: ${JSON.stringify(value)}\n\n`;
const shapes = {
  chat: {
    buffered: (model, text) => JSON.stringify({ id: 'chat-matrix', object: 'chat.completion', created: 1, model,
      choices: [{ index: 0, message: { role: 'assistant', content: text }, finish_reason: 'stop' }],
      usage: { prompt_tokens: 2, completion_tokens: 3, total_tokens: 5 } }),
    stream: (model, text) => {
      const chunk = (delta, finish, usage) => sse('', { id: 'chat-matrix', object: 'chat.completion.chunk', created: 1, model,
        choices: [{ index: 0, delta, finish_reason: finish }], ...(usage ? { usage } : {}) });
      return {
        prefix: () => chunk({ role: 'assistant', content: text }, null),
        delta: more => chunk({ content: more }, null),
        tail: () => chunk({}, 'stop', { prompt_tokens: 2, completion_tokens: 3, total_tokens: 5 }) + 'data: [DONE]\n\n',
      };
    },
  },
  messages: {
    buffered: (model, text) => JSON.stringify({ id: 'message-matrix', type: 'message', role: 'assistant', model,
      content: [{ type: 'text', text }], stop_reason: 'end_turn', stop_sequence: null,
      usage: { input_tokens: 2, output_tokens: 3 } }),
    stream: (model, text) => ({
      prefix: () => sse('message_start', { type: 'message_start', message: { id: 'message-matrix', type: 'message', role: 'assistant',
          model, content: [], stop_reason: null, stop_sequence: null, usage: { input_tokens: 2, output_tokens: 0 } } })
        + sse('content_block_start', { type: 'content_block_start', index: 0, content_block: { type: 'text', text: '' } })
        + sse('content_block_delta', { type: 'content_block_delta', index: 0, delta: { type: 'text_delta', text } }),
      delta: more => sse('content_block_delta', { type: 'content_block_delta', index: 0, delta: { type: 'text_delta', text: more } }),
      tail: () => sse('content_block_stop', { type: 'content_block_stop', index: 0 })
        + sse('message_delta', { type: 'message_delta', delta: { stop_reason: 'end_turn', stop_sequence: null }, usage: { output_tokens: 3 } })
        + sse('message_stop', { type: 'message_stop' }),
    }),
  },
  responses: (() => {
    const item = text => ({ id: 'msg_matrix', type: 'message', status: 'completed', role: 'assistant',
      content: [{ type: 'output_text', text, annotations: [] }] });
    const usage = { input_tokens: 2, output_tokens: 3, total_tokens: 5 };
    const response = (model, output, status) => ({ id: 'resp_matrix', object: 'response', created_at: 1, model, status,
      output, usage: status === 'completed' ? usage : null, incomplete_details: null, error: null });
    return {
      buffered: (model, text) => JSON.stringify(response(model, [item(text)], 'completed')),
      stream: (model, text) => {
        let sequence = 0;
        const event = (type, fields) => sse(type, { type, sequence_number: sequence++, ...fields });
        const owner = { item_id: 'msg_matrix', output_index: 0, content_index: 0 };
        return {
          prefix: () => event('response.created', { response: response(model, [], 'in_progress') })
            + event('response.output_item.added', { output_index: 0, item: { id: 'msg_matrix', type: 'message', role: 'assistant', status: 'in_progress', content: [] } })
            + event('response.content_part.added', { ...owner, part: { type: 'output_text', text: '', annotations: [] } })
            + event('response.output_text.delta', { ...owner, delta: text }),
          delta: more => event('response.output_text.delta', { ...owner, delta: more }),
          tail: () => event('response.output_text.done', { ...owner, text })
            + event('response.content_part.done', { ...owner, part: { type: 'output_text', text, annotations: [] } })
            + event('response.output_item.done', { output_index: 0, item: item(text) })
            + event('response.completed', { response: response(model, [item(text)], 'completed') }),
        };
      },
    };
  })(),
  gemini: (() => {
    const body = (model, parts, reason, usage) => ({ modelVersion: model, responseId: 'generation',
      candidates: [{ index: 0, content: { role: 'model', parts }, ...(reason ? { finishReason: reason } : {}) }],
      ...(usage ? { usageMetadata: usage } : {}) });
    const frame = value => `data: ${JSON.stringify(value)}\n\n`;
    return {
      buffered: (model, text) => JSON.stringify(body(model, [{ text }], 'STOP', usageGoogle)),
      stream: (model, text) => ({
        prefix: () => frame(body(model, [{ text }])),
        delta: more => frame(body(model, [{ text: more }])),
        tail: () => frame(body(model, [], 'STOP', usageGoogle)),
      }),
    };
  })(),
  interactions: (() => {
    const usage = { total_input_tokens: 2, total_output_tokens: 3, total_tokens: 5 };
    const step = text => ({ type: 'model_output', content: [{ type: 'text', text, annotations: [] }] });
    const resource = (model, text) => ({ id: 'interaction_matrix', model, status: 'completed', steps: [step(text)], usage });
    const event = (event_type, fields) => sse(event_type, { event_type, ...fields });
    return {
      buffered: (model, text) => JSON.stringify(resource(model, text)),
      stream: (model, text) => ({
        prefix: () => event('interaction.created', { interaction: { id: 'interaction_matrix', model, status: 'in_progress' } })
          + event('step.start', { index: 0, step: { type: 'model_output', content: [] } })
          + event('step.delta', { index: 0, delta: { type: 'text', text } }),
        delta: more => event('step.delta', { index: 0, delta: { type: 'text', text: more } }),
        tail: () => event('step.stop', { index: 0 }) + event('interaction.completed', { interaction: resource(model, text) })
          + 'event: done\ndata: [DONE]\n\n',
      }),
    };
  })(),
};

// Family-shaped error envelopes. `err` names the failure class; each family's vendor spelling is
// the one the runtime's error policy (config/error-policy.json) is written against.
const google = {
  invalid_request: ['INVALID_ARGUMENT'], auth: ['UNAUTHENTICATED'], permission: ['PERMISSION_DENIED'],
  not_found: ['NOT_FOUND'], rate_limit: ['RESOURCE_EXHAUSTED'], server: ['INTERNAL'], overloaded: ['UNAVAILABLE'],
  api_key_invalid: ['INVALID_ARGUMENT', 'API_KEY_INVALID'],
};
const anthropic = {
  invalid_request: 'invalid_request_error', auth: 'authentication_error', permission: 'permission_error',
  not_found: 'not_found_error', rate_limit: 'rate_limit_error', server: 'api_error', overloaded: 'overloaded_error',
};
const openai = { // [type, code]
  invalid_request: ['invalid_request_error', null], auth: ['invalid_request_error', 'invalid_api_key'],
  permission: ['invalid_request_error', 'insufficient_permissions'], not_found: ['invalid_request_error', 'model_not_found'],
  rate_limit: ['requests', 'rate_limit_exceeded'], server: ['server_error', null], overloaded: ['server_error', 'overloaded'],
  api_key_invalid: ['invalid_request_error', 'invalid_api_key'],
};
function errorBody(family, err, status) {
  if (family === 'messages') return { type: 'error', error: { type: anthropic[err] ?? anthropic.server, message: key } };
  if (family === 'gemini' || family === 'interactions') {
    const [name, reason] = google[err] ?? google.server;
    return { error: { code: status, message: key, status: name,
      ...(reason ? { details: [{ '@type': 'type.googleapis.com/google.rpc.ErrorInfo', reason, domain: 'googleapis.com' }] } : {}) } };
  }
  const [type, code] = openai[err] ?? openai.server;
  return { error: { message: key, type, code, param: null } };
}

// ---- request identification --------------------------------------------------------------------
function identify(req) {
  switch (req.url) {
    case '/v1/chat/completions': return { family: 'chat' };
    case '/v1/responses': return { family: 'responses' };
    case '/v1/messages': return { family: 'messages' };
    case '/v1beta/interactions': return { family: 'interactions' };
  }
  const m = /^\/v1beta\/models\/([^/:?]+):(generateContent|streamGenerateContent\?alt=sse)$/.exec(req.url);
  return m ? { family: 'gemini', model: m[1], stream: m[2] !== 'generateContent' } : undefined;
}
const authorized = (family, h) => family === 'messages' ? h['x-api-key'] === key
  : family === 'gemini' || family === 'interactions' ? h['x-goog-api-key'] === key && !h.authorization
    : h.authorization === `Bearer ${key}`;
async function readBody(req) {
  const chunks = []; let size = 0;
  for await (const chunk of req) { size += chunk.length; if (size > (1 << 20)) throw new Error('request bound'); chunks.push(chunk); }
  return Buffer.concat(chunks).toString('utf8');
}

// ---- actions -----------------------------------------------------------------------------------
const NGHTTP2_CANCEL = http2.constants.NGHTTP2_CANCEL, NGHTTP2_INTERNAL_ERROR = http2.constants.NGHTTP2_INTERNAL_ERROR;
function httpDate(seconds) { return new Date(Date.now() + seconds * 1000).toUTCString(); }
async function perform(c, a, x) {
  const { res, family, model, stream, h2 } = x;
  const type = stream ? 'text/event-stream' : 'application/json';
  const text = a.text ?? 'hello';
  const shape = shapes[family];
  const wire = stream ? shape.stream(model, text) : undefined;
  const head = (status, extra = {}) => { res.writeHead(status, { 'content-type': type, ...extra }); res.flushHeaders(); };
  const send = data => { c.bytes += Buffer.byteLength(data); res.write(data); };
  const fault = () => { ++c.faults; notify(); };
  const kill = () => { if (h2) res.stream.close(NGHTTP2_CANCEL); else raw.destroy(); };
  // Retain the real TCP socket at connection acceptance; HTTP/2 exposes only a session proxy.
  const raw = tcpSockets.get(tcpKey(res.socket));
  if (!raw) throw new Error('untracked TCP connection');
  const rst = () => raw.resetAndDestroy();
  const abort = () => { if (h2) res.stream.close(NGHTTP2_INTERNAL_ERROR); else rst(); };
  const hold = release => { c.held.set(res, release ?? kill); notify(); };
  // The first bytes of a body: one complete SSE prefix, or the first half of the buffered JSON.
  const partial = () => { if (stream) return wire.prefix(); const whole = shape.buffered(model, text); return whole.slice(0, whole.length >> 1); };
  switch (a.a) {
    case 'ok': {
      const data = stream ? wire.prefix() + wire.tail() : shape.buffered(model, text);
      c.bytes += Buffer.byteLength(data);
      res.writeHead(200, { 'content-type': type });
      res.end(data);
      if (a.then === 'reset-idle') {
        // The client requests this fault only after consuming the completed response.
        // Reset the owned TCP socket; HTTP/2 session.destroy() would send orderly GOAWAY.
        c.idleReset = () => { raw.once('close', fault); rst(); };
      }
      return;
    }
    case 'status': {
      const headers = { 'content-type': a.body === 'html' ? 'text/html' : 'application/json' };
      if (a.retry_after !== undefined) headers['retry-after'] = String(a.retry_after);
      if (a.retry_after_date_s !== undefined) headers['retry-after'] = httpDate(a.retry_after_date_s);
      let data = '';
      if (a.body === 'html') data = '<html><body>Bad Gateway</body></html>';
      else if (a.body !== 'empty') {
        const envelope = errorBody(family, a.err ?? 'server', a.status);
        data = JSON.stringify(a.body === 'array' ? [envelope] : envelope);
      }
      fault();
      res.writeHead(a.status, headers);
      res.end(data);
      return;
    }
    case 'stall-head': hold(); return;
    case 'stall-after-head': head(200); hold(); return;
    case 'stall-mid':
      head(200); send(partial());
      if (a.partial_sse && stream) {
        // Hold inside the next data line: the preceding complete prefix must survive,
        // but this incomplete JSON/SSE frame must never produce a semantic delta.
        const next = wire.delta('uncommitted');
        send(next.slice(0, next.indexOf('data: ') + 8));
      }
      hold(); return;
    case 'trickle': {
      head(200); send(partial());
      const timer = setInterval(() => { if (res.destroyed || res.writableEnded) clearInterval(timer); else send(wire.delta('x')); }, a.interval_ms ?? 40);
      res.on('close', () => clearInterval(timer));
      hold(); return;
    }
    case 'close-after-head':
      // HTTP/1.1: the head promises more bytes (chunked or Content-Length) and the peer closes. HTTP/2: END_STREAM
      // right after HEADERS, a well-framed empty body.
      if (h2) { head(200); fault(); res.end(); return; }
      head(200, stream ? {} : { 'content-length': '4096' });
      fault(); raw.end(); return;
    case 'reset':
      if (a.at === 'body') {
        head(200); send(partial());
        // Let the client consume the output first, then reset: released by the controller.
        if (a.hold) { hold(() => { fault(); abort(); }); return; }
        setTimeout(() => { fault(); abort(); }, 30);
        return;
      }
      fault(); abort();
      return;
    default: throw new Error('unknown action');
  }
}

async function handle(req, res, tls) {
  let c;
  try {
    const raw = await readBody(req);
    const known = identify(req);
    let body;
    try { body = JSON.parse(raw); } catch { body = undefined; }
    const model = known?.model ?? body?.model;
    c = known && model ? cases.get(model) : undefined;
    if (!c) { ++unexpected; res.writeHead(400); res.end(); return; }
    const h2 = req.httpVersionMajor === 2;
    const stream = known.stream ?? body?.stream;
    ++c.count; c.times.push(now());
    if (h2) ++c.h2; if (tls) ++c.tls;
    const connection = h2 ? req.stream.session : req.socket;
    if (!ids.has(connection)) ids.set(connection, ++nextId);
    if (c.conns.has(ids.get(connection))) ++c.reused;
    c.conns.add(ids.get(connection));
    if (req.method !== 'POST' || known.family !== c.family || !authorized(known.family, req.headers)
        || body === undefined || typeof stream !== 'boolean' || stream !== c.streaming) ++c.invalid;
    res.on('close', () => { ++c.closed; c.held.delete(res); notify(); });
    notify();
    const action = c.plan[Math.min(c.count, c.plan.length) - 1];
    await perform(c, action, { req, res, family: known.family, model, stream: !!stream, h2 });
    notify();
  } catch {
    if (c) ++c.invalid; else ++unexpected;
    try { res.destroy(); } catch { /* already gone */ }
    notify();
  }
}

// ---- listeners ---------------------------------------------------------------------------------
function openssl(...args) {
  const result = spawnSync(process.env.SP_OPENSSL || 'openssl', args, { cwd: directory, stdio: ['ignore', 'pipe', 'pipe'], timeout: 10000, killSignal: 'SIGKILL' });
  if (result.error || result.status !== 0) throw new Error(`openssl ${args[0]} failed`);
}
function track(server) {
  server.on('connection', socket => {
    const key = tcpKey(socket);
    tcpSockets.set(key, socket); sockets.add(socket); socket.on('error', () => {});
    socket.once('close', () => {
      sockets.delete(socket);
      if (tcpSockets.get(key) === socket) tcpSockets.delete(key);
    });
  });
  server.on('secureConnection', socket => { socket.on('error', () => {}); });
  server.on('clientError', (_error, socket) => socket.destroy());
  server.on('tlsClientError', (_error, socket) => socket.destroy());
  server.on('session', session => session.on('error', () => {}));
  server.on('error', () => process.exit(1));
  return server;
}
const listen = server => new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
function cleanup() { if (directory) { rmSync(directory, { recursive: true, force: true }); directory = undefined; } }
process.on('exit', cleanup);
for (const signal of ['SIGINT', 'SIGTERM']) process.on(signal, () => process.exit(0));
// A scripted fault that throws must show up as an `unexpected` counter, not take the whole peer down.
process.on('uncaughtException', () => { ++unexpected; notify(); });

directory = mkdtempSync(join(tmpdir(), 'schemaprovider-matrix-'));
openssl('req', '-x509', '-newkey', 'ec', '-pkeyopt', 'ec_paramgen_curve:prime256v1', '-nodes', '-sha256',
  '-keyout', 'server.key', '-out', 'server.pem', '-days', '2', '-subj', '/CN=localhost',
  '-addext', 'basicConstraints=critical,CA:TRUE', '-addext', 'keyUsage=critical,digitalSignature,keyCertSign',
  '-addext', 'extendedKeyUsage=serverAuth', '-addext', 'subjectAltName=DNS:localhost,IP:127.0.0.1');
const tlsOptions = { key: readFileSync(join(directory, 'server.key')), cert: readFileSync(join(directory, 'server.pem')), allowHTTP1: true };
const plain = track(http.createServer((req, res) => handle(req, res, false)));
const h2c = track(http2.createServer((req, res) => handle(req, res, false)));
const secure = track(http2.createSecureServer(tlsOptions, (req, res) => handle(req, res, true)));
const blackhole = net.createServer(socket => { ++blackholeConnections; sockets.add(socket); socket.on('error', () => {}); socket.once('close', () => sockets.delete(socket)); notify(); });
blackhole.on('error', () => process.exit(1));
await Promise.all([plain, h2c, secure, blackhole].map(listen));
reply({ port: plain.address().port });

const control = readline.createInterface({ input: process.stdin });
control.on('line', async line => {
  try {
    const command = JSON.parse(line);
    if (command.info) {
      reply({ plain: plain.address().port, h2c: h2c.address().port, tls: secure.address().port,
        blackhole: blackhole.address().port, ca: join(directory, 'server.pem') });
      return;
    }
    if (command.arm) {
      if (cases.has(command.arm) || !Array.isArray(command.plan) || command.plan.length === 0
          || typeof command.streaming !== 'boolean') throw new Error('bad arm');
      cases.set(command.arm, { family: command.family, streaming: command.streaming, plan: command.plan, count: 0, times: [], held: new Map(), closed: 0,
        faults: 0, invalid: 0, bytes: 0, h2: 0, tls: 0, conns: new Set(), reused: 0 });
      reply({ armed: true }); return;
    }
    const c = cases.get(command.model);
    if (!c) throw new Error('unknown model');
    if (command.reset_idle) {
      if (typeof c.idleReset !== 'function') throw new Error('idle reset not armed');
      const reset = c.idleReset; delete c.idleReset; reset();
    }
    if (command.wait) await new Promise((resolve, reject) => {
      const ready = () => {
        const s = snapshot(c);
        if (Object.entries(command.wait).every(([name, minimum]) => s[name] >= minimum)) { clearTimeout(timer); waiters.delete(ready); resolve(); }
      };
      const timer = setTimeout(() => { waiters.delete(ready); reject(new Error('peer wait timeout')); }, 12000);
      waiters.add(ready); ready();
    });
    if (command.release) { const held = [...c.held.values()]; c.held.clear(); for (const release of held) { try { release(); } catch { ++c.invalid; } } notify(); }
    reply(snapshot(c));
  } catch { reply({ error: 'lifecycle peer control failure' }); }
});
control.on('close', () => process.exit(0));
