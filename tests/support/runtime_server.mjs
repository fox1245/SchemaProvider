// Model-free runtime oracle. Control replies contain counters, never request headers/bodies.
import http from 'node:http';
import readline from 'node:readline';
import { once } from 'node:events';

const marker = 'RUNTIME_SECRET_MARKER_62c490';
const cases = new Map();
const sockets = new Set();
const waiters = new Set();
const reply = value => process.stdout.write(`${JSON.stringify(value)}\n`);
const now = () => Number(process.hrtime.bigint()) / 1e6;
let unexpected = 0;
function notify() { for (const wake of [...waiters]) wake(); }
function snapshot(c) {
  return { count: c.count, times: c.times, held: c.held.size, closed: c.closed,
    faults: c.faults, bytes: c.bytes, invalid: c.invalid, unexpected };
}
function frame(type, value, messages) {
  return `${messages ? `event: ${type}\n` : ''}data: ${JSON.stringify(value)}\n\n`;
}
function wire(c, model, streaming, messages) {
  const text = c.text ?? (c.scenario === 'burst' || c.scenario === 'burst-held'
    ? Array.from({ length: 2048 }, (_, index) => index.toString().padStart(8, '0') + ':' + 'x'.repeat(4096 - 9)).join('')
    : 'hello');
  const content = c.scenario === 'reasoning'
    ? [{ type: 'thinking', thinking: marker, signature: 'synthetic-signature' }, { type: 'text', text }]
    : c.scenario === 'parts'
      ? [{ type: 'text', text: 'one' }, { type: 'text', text: 'two' }]
      : c.scenario === 'tool'
        ? [{ type: 'tool_use', id: 'tool-1', name: 'synthetic', input: { value: 'x'.repeat(2048) } }]
        : [{ type: 'text', text }];
  if (!streaming) {
    if (messages) return JSON.stringify({ id: 'message-runtime', type: 'message', role: 'assistant', model,
      content, stop_reason: c.scenario === 'tool' ? 'tool_use' : 'end_turn', stop_sequence: null,
      usage: { input_tokens: 2, output_tokens: 3 } });
    return JSON.stringify({ id: 'chat-runtime', object: 'chat.completion', created: 1, model,
      choices: [{ index: 0, message: { role: 'assistant', content: text }, finish_reason: 'stop' }],
      usage: { prompt_tokens: 2, completion_tokens: 3, total_tokens: 5 } });
  }
  if (!messages) return frame('', { id: 'chat-runtime', object: 'chat.completion.chunk', created: 1, model,
    choices: [{ index: 0, delta: { role: 'assistant', content: text }, finish_reason: null }] }, false)
    + frame('', { id: 'chat-runtime', object: 'chat.completion.chunk', created: 1, model,
      choices: [{ index: 0, delta: {}, finish_reason: 'stop' }], usage: { prompt_tokens: 2, completion_tokens: 3, total_tokens: 5 } }, false)
    + 'data: [DONE]\n\n';
  let body = frame('message_start', { type: 'message_start', message: { id: 'message-runtime', type: 'message',
    role: 'assistant', model, content: [], stop_reason: null, stop_sequence: null,
    usage: { input_tokens: 2, output_tokens: 0 } } }, true);
  for (const [index, block] of content.entries()) {
    body += frame('content_block_start', { type: 'content_block_start', index, content_block: block.type === 'text'
      ? { type: 'text', text: '' } : block }, true);
    if (block.type === 'text') body += frame('content_block_delta', { type: 'content_block_delta', index,
      delta: { type: 'text_delta', text: block.text } }, true);
    body += frame('content_block_stop', { type: 'content_block_stop', index }, true);
  }
  return body + frame('message_delta', { type: 'message_delta', delta: { stop_reason: c.scenario === 'tool' ? 'tool_use' : 'end_turn',
    stop_sequence: null }, usage: { output_tokens: 3 } }, true)
    + frame('message_stop', { type: 'message_stop' }, true);
}
function prefix(model, messages) {
  if (!messages) return frame('', { id: 'chat-runtime', object: 'chat.completion.chunk', created: 1, model,
    choices: [{ index: 0, delta: { role: 'assistant', content: 'partial' }, finish_reason: null }] }, false);
  return frame('message_start', { type: 'message_start', message: { id: 'message-runtime', type: 'message', role: 'assistant',
    model, content: [], stop_reason: null, stop_sequence: null, usage: { input_tokens: 2, output_tokens: 0 } } }, true)
    + frame('content_block_start', { type: 'content_block_start', index: 0, content_block: { type: 'text', text: '' } }, true)
    + frame('content_block_delta', { type: 'content_block_delta', index: 0, delta: { type: 'text_delta', text: 'partial' } }, true);
}
const server = http.createServer(async (req, res) => {
  let c;
  try {
    let size = 0;
    const chunks = [];
    for await (const chunk of req) {
      size += chunk.length;
      if (size > (1 << 20)) throw new Error('request bound');
      chunks.push(chunk);
    }
    const body = JSON.parse(Buffer.concat(chunks).toString('utf8'));
    c = cases.get(body.model);
    if (!c) { ++unexpected; res.writeHead(400); res.end(); return; }
    ++c.count; c.times.push(now());
    const messages = req.url === '/v1/messages';
    if (req.method !== 'POST' || (!messages && req.url !== '/v1/chat/completions')
        || typeof body.stream !== 'boolean' || !Array.isArray(body.messages)
        || (messages ? req.headers['x-api-key'] !== marker : req.headers.authorization !== `Bearer ${marker}`)) ++c.invalid;
    res.on('close', () => { ++c.closed; c.held.delete(res); notify(); });
    notify();
    const scenario = c.scenario;
    const fail = scenario === 'always-error' || scenario === 'quota' || scenario === 'vendor-secret' || scenario === 'error-cap'
      || scenario === 'rate' || scenario === 'long-rate' || scenario === 'malformed-rate'
      || ((scenario === 'recover' || scenario === 'retry-held') && c.count === 1);
    if (fail) {
      const status = scenario === 'quota' || scenario.includes('rate') ? 429 : 503;
      const type = scenario === 'quota' ? (messages ? 'billing_error' : 'insufficient_quota')
        : scenario === 'vendor-secret' ? marker : status === 429 ? (messages ? 'rate_limit_error' : 'rate_limit_exceeded')
          : messages ? 'overloaded_error' : 'server_error';
      const headers = { 'Content-Type': 'application/json', Connection: 'close' };
      if (scenario === 'rate') { headers['retry-after'] = '1'; headers['retry-after-ms'] = '1250'; }
      if (scenario === 'long-rate') headers['retry-after'] = '30';
      if (scenario === 'malformed-rate') headers['retry-after'] = '999999999999999999999999999';
      if (scenario === 'retry-held') headers['retry-after'] = '1';
      res.writeHead(status, headers);
      res.end(JSON.stringify({ type: 'error', error: { type, code: type, message: scenario === 'error-cap' ? marker.repeat(4096) : marker } }));
      ++c.faults; notify(); return;
    }
    if (scenario === 'reset' || (scenario === 'reset-recover' && c.count === 1)) {
      ++c.faults; res.socket.resetAndDestroy(); notify(); return;
    }
    const streaming = body.stream;
    const data = scenario === 'quota-short-close'
      ? JSON.stringify({ error: { code: 'insufficient_quota', message: marker } })
      : wire(c, body.model, streaming, messages);
    const headers = { 'Content-Type': streaming ? 'text/event-stream' : 'application/json', Connection: 'close' };
    if (scenario === 'header-cap') headers['x-padding'] = 'x'.repeat(8192);
    if (scenario === 'short-close' || scenario === 'quota-short-close') headers['Content-Length'] = Buffer.byteLength(data) + 17;
    res.writeHead(200, headers);
    res.flushHeaders();
    const send = async bytes => {
      c.bytes += Buffer.byteLength(bytes);
      if (!res.write(bytes)) await Promise.race([once(res, 'drain'), once(res, 'close')]);
    };
    if (scenario === 'hold' || scenario === 'slow' || scenario === 'retry-held' || scenario === 'burst-held') {
      c.held.set(res, () => { c.bytes += Buffer.byteLength(data); res.end(data); }); notify(); return;
    }
    if (scenario === 'partial' || scenario === 'partial-error') {
      await send(prefix(body.model, messages));
      if (scenario === 'partial-error') {
        await send(frame('error', { type: 'error', error: {
          type: messages ? 'overloaded_error' : 'rate_limit_exceeded', message: marker } }, messages));
        ++c.faults; res.end(); notify(); return;
      }
      c.held.set(res, () => { ++c.faults; res.destroy(); notify(); }); notify(); return;
    }
    if (scenario === 'ping-recover' && c.count === 1) {
      await send(messages ? frame('ping', { type: 'ping' }, true) : ': keepalive\n\n');
      await send(frame('error', { type: 'error', error: {
        type: messages ? 'overloaded_error' : 'rate_limit_exceeded', message: marker } }, messages));
      ++c.faults; res.end(); notify(); return;
    }
    if (scenario === 'flood') {
      for (let i = 0; i < 512 && !res.destroyed; ++i) await send(': ' + 'x'.repeat(4096) + '\n\n');
      if (!res.destroyed) res.end(data);
      return;
    }
    await send(data);
    if (scenario === 'close-gate') { c.held.set(res, () => res.end()); notify(); return; }
    if (scenario === 'short-close' || scenario === 'quota-short-close') { ++c.faults; res.socket.end(); notify(); return; }
    res.end();
  } catch {
    if (c) ++c.invalid;
    else ++unexpected;
    res.destroy(); notify();
  }
});
server.on('connection', socket => { sockets.add(socket); socket.on('close', () => sockets.delete(socket)); });
server.on('clientError', (_error, socket) => socket.destroy());
server.listen(0, '127.0.0.1', () => reply({ port: server.address().port }));
const control = readline.createInterface({ input: process.stdin });
control.on('line', async line => {
  try {
    const command = JSON.parse(line);
    if (command.arm) {
      if (cases.has(command.arm)) throw new Error('duplicate model');
      cases.set(command.arm, { scenario: command.scenario, text: command.text, count: 0, times: [], held: new Map(),
        closed: 0, faults: 0, bytes: 0, invalid: 0 });
      reply({ armed: true }); return;
    }
    const c = cases.get(command.model);
    if (!c) throw new Error('unknown model');
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
    if (command.release) {
      const releases = [...c.held.values()]; c.held.clear();
      for (const release of releases) release();
      notify();
    }
    reply(snapshot(c));
  } catch { reply({ error: 'runtime peer control failure' }); }
});
control.on('close', () => { server.close(); for (const socket of sockets) socket.destroy(); });
