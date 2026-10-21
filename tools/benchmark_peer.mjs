// Independent model-free TLS protocol oracle; no production encoder imports.
import http2 from 'node:http2';
import https from 'node:https';
import cluster from 'node:cluster';
import readline from 'node:readline';
import { mkdtempSync, readFileSync, writeFileSync, rmSync } from 'node:fs';
import { join } from 'node:path';
import { spawnSync } from 'node:child_process';
import { isDeepStrictEqual as equal } from 'node:util';

const args = process.argv.slice(2);
if (args.length !== 2 || args[0] !== '--config') throw new Error('usage: benchmark_peer.mjs --config file.json');
const config = JSON.parse(readFileSync(args[1], 'utf8'));
for (const key of ['payload_bytes', 'server_delay_ms']) {
  if (!Number.isSafeInteger(config[key]) || config[key] < 0) throw new Error(`invalid ${key}`);
}
// Optional keys used by benchmarks/sdk_comparison; every default keeps the original single-frame behaviour.
//   response_bytes  length of the generated text (default payload_bytes)
//   stream_chunks   number of text deltas of a streamed Chat or Messages reply (default 1)
//   sse_flush       'single' sends the stream in one write, 'event' writes every SSE event separately
//   alpn            'both' offers h2 and http/1.1, 'h1' offers http/1.1 only
//   expect_streaming, api_key, system_text   when present, a request that differs is rejected
//   workers         serve from this many processes sharing one port (default 1); statistics and the barrier span all of them
//   warm_barrier    hold the replies to the first N requests until N are in flight at once, so that a warm-up opens N connections
//   debug_rejections  write the reason and the head of the body of every rejected request to stderr
for (const key of ['response_bytes', 'stream_chunks']) {
  if (config[key] !== undefined && (!Number.isSafeInteger(config[key]) || config[key] < (key === 'stream_chunks' ? 1 : 0))) throw new Error(`invalid ${key}`);
}
if (!['single', 'event'].includes(config.sse_flush ?? 'single')) throw new Error('invalid sse_flush');
if (config.workers !== undefined && (!Number.isSafeInteger(config.workers) || config.workers < 1)) throw new Error('invalid workers');
if (!['both', 'h1'].includes(config.alpn ?? 'both')) throw new Error('invalid alpn');
const chunkCount = config.stream_chunks ?? 1, flushPerEvent = config.sse_flush === 'event';
let barrierHeld = [], barrierOpen = !(config.warm_barrier > 1), barrierTimer;
function openBarrier() { barrierOpen = true; clearTimeout(barrierTimer); for (const resume of barrierHeld) resume(); barrierHeld = []; }
function barrier() {
  if (barrierOpen) return undefined;
  return new Promise(resolve => {
    barrierHeld.push(resolve);
    if (cluster.isWorker) process.send({ type: 'hold' });
    else if (barrierHeld.length >= config.warm_barrier) openBarrier();
    else barrierTimer ??= setTimeout(() => { ++stats.barrier_timeouts; openBarrier(); }, 15000);
  });
}
const text = 'benchmark-ok:' + 'x'.repeat(config.response_bytes ?? config.payload_bytes);
function pieces(value, n) {
  if (value.length < n) throw new Error('stream_chunks exceeds the text length');
  const out = [], size = Math.floor(value.length / n), extra = value.length % n;
  for (let i = 0, at = 0; i < n; i++) { const end = at + size + (i < extra ? 1 : 0); out.push(value.slice(at, end)); at = end; }
  return out;
}
const toolArgs = { value: 'bench' }, toolName = 'bench_echo', callId = 'bench-call';
const signature = 'BENCHMARK_PUBLIC_SYNTHETIC_SIGNATURE';
// Public synthetic encrypted carrier: replay fidelity only, not vendor-valid cryptography.
const chatReasoning = () => [{ id: 'bench-reasoning', type: 'reasoning.encrypted',
  data: signature, format: 'benchmark.synthetic', index: 0 }];
const count = () => ({ requests: 0, h1: 0, h2: 0, invalid: 0, continuations: 0,
  native_replays: 0, request_bytes: 0, response_bytes: 0 });
const stats = { version: 1, ...count(), connections: 0, active_connections: 0, tls_errors: 0, barrier_timeouts: 0, cases: {}, observed: { body_keys: {}, header_names: {}, user_agents: {} } };
const observe = (table, name) => { if (name in table || Object.keys(table).length < 32) table[name] = (table[name] ?? 0) + 1; };
const sockets = new Set(), sessions = new Set();
let directory, server, stopping = false;
const reply = value => process.stdout.write(JSON.stringify(value) + '\n');
const snapshot = () => { const cpu = process.cpuUsage(); return { ...stats, active_connections: sockets.size, cpu_user_us: cpu.user, cpu_system_us: cpu.system }; };
function cleanup() { if (directory) { rmSync(directory, { recursive: true, force: true }); directory = undefined; } }
function shutdown(code = 0) {
  if (stopping) return; stopping = true;
  for (const session of sessions) session.destroy();
  for (const socket of sockets) socket.destroy();
  server?.close(); process.exit(code);
}
process.on('exit', cleanup);
process.on('SIGTERM', () => shutdown()); process.on('SIGINT', () => shutdown());
function openssl(...argv) {
  const run = spawnSync('openssl', argv, { cwd: directory, timeout: 10000, maxBuffer: 1 << 20, stdio: ['ignore', 'pipe', 'pipe'] });
  if (run.error || run.status !== 0) throw new Error('ephemeral certificate generation failed');
}
const data = obj => `data: ${JSON.stringify(obj)}\n\n`;
const event = (name, obj) => `event: ${name}\ndata: ${JSON.stringify(obj)}\n\n`;
const usage = { input_tokens: 10, output_tokens: 7, total_tokens: 17 };
const responseMessage = value => ({ id: 'bench-message', type: 'message', status: 'completed', role: 'assistant', content: [{ type: 'output_text', text: value, annotations: [] }] });
const responseReasoning = () => ({ id: 'bench-reasoning', type: 'reasoning', status: 'completed', summary: [{ type: 'summary_text', text: 'synthetic reasoning' }], encrypted_content: signature });
const responseCall = () => ({ id: 'bench-function', type: 'function_call', status: 'completed', call_id: callId, name: toolName, arguments: JSON.stringify(toolArgs) });
const interactionThought = () => ({ type: 'thought', signature, summary: [{ type: 'text', text: 'synthetic reasoning' }], future: { preserved: true } });
const interactionCall = () => ({ type: 'function_call', id: callId, name: toolName, arguments: toolArgs });
const interactionText = value => ({ type: 'model_output', content: [{ type: 'text', text: value, annotations: [] }] });
const messagesThinking = () => ({ type: 'thinking', thinking: 'synthetic reasoning', signature });
const messagesCall = () => ({ type: 'tool_use', id: callId, name: toolName, input: toolArgs });
const geminiThought = () => ({ thought: true, text: 'synthetic reasoning', thoughtSignature: signature });
const geminiCall = native => ({ functionCall: { id: callId, name: toolName, args: toolArgs }, ...(native ? { thoughtSignature: signature } : {}) });

function fixture(family, kind, continuation, streaming) {
  const tool = kind !== 'text' && !continuation, native = kind === 'native' && !continuation;
  if (family === 'openai.chat') {
    const msg = tool ? { role: 'assistant', content: null, ...(native ? { reasoning_details: chatReasoning() } : {}), tool_calls: [{ id: callId, type: 'function', function: { name: toolName, arguments: JSON.stringify(toolArgs) } }] } : { role: 'assistant', content: text };
    const base = { id: 'bench-chat', object: 'chat.completion', created: 1, model: 'bench-model' };
    const counters = { prompt_tokens: 10, completion_tokens: 7, total_tokens: 17 };
    if (!streaming) return JSON.stringify({ ...base, choices: [{ index: 0, message: msg, finish_reason: tool ? 'tool_calls' : 'stop' }], usage: counters });
    if (!tool && chunkCount > 1) {
      const chunk = (delta, finish_reason, extra) => data({ ...base, object: 'chat.completion.chunk', choices: [{ index: 0, delta, finish_reason }], ...extra });
      return chunk({ role: 'assistant', content: '' }, null) + pieces(text, chunkCount).map(piece => chunk({ content: piece }, null)).join('')
        + chunk({}, 'stop', { usage: counters }) + 'data: [DONE]\n\n';
    }
    const delta = tool ? { role: 'assistant', ...(native ? { reasoning_details: chatReasoning() } : {}), tool_calls: [{ index: 0, ...msg.tool_calls[0] }] } : msg;
    return data({ ...base, object: 'chat.completion.chunk', choices: [{ index: 0, delta, finish_reason: null }] })
      + data({ ...base, object: 'chat.completion.chunk', choices: [{ index: 0, delta: {}, finish_reason: tool ? 'tool_calls' : 'stop' }], usage: counters }) + 'data: [DONE]\n\n';
  }
  if (family === 'anthropic.messages') {
    const content = tool ? [...(native ? [messagesThinking()] : []), messagesCall()] : [{ type: 'text', text }];
    const stop = tool ? 'tool_use' : 'end_turn';
    const resource = { id: 'bench-message', type: 'message', role: 'assistant', model: 'bench-model', content, stop_reason: stop, stop_sequence: null, usage: { input_tokens: 10, output_tokens: 7 } };
    if (!streaming) return JSON.stringify(resource);
    let wire = event('message_start', { type: 'message_start', message: { ...resource, content: [], stop_reason: null, usage: { input_tokens: 10, output_tokens: 0 } } });
    if (!tool && chunkCount > 1) {
      wire += event('content_block_start', { type: 'content_block_start', index: 0, content_block: { type: 'text', text: '' } });
      wire += event('ping', { type: 'ping' });
      for (const piece of pieces(text, chunkCount)) wire += event('content_block_delta', { type: 'content_block_delta', index: 0, delta: { type: 'text_delta', text: piece } });
      wire += event('content_block_stop', { type: 'content_block_stop', index: 0 });
      return wire + event('message_delta', { type: 'message_delta', delta: { stop_reason: stop, stop_sequence: null }, usage: { output_tokens: 7 } }) + event('message_stop', { type: 'message_stop' });
    }
    for (const [index, block] of content.entries()) {
      const initial = block.type === 'thinking' ? { type: 'thinking', thinking: '', signature: '' } : block.type === 'tool_use' ? { ...block, input: {} } : { type: 'text', text: '' };
      wire += event('content_block_start', { type: 'content_block_start', index, content_block: initial });
      const delta = block.type === 'thinking' ? { type: 'thinking_delta', thinking: block.thinking } : block.type === 'tool_use' ? { type: 'input_json_delta', partial_json: JSON.stringify(toolArgs) } : { type: 'text_delta', text };
      wire += event('content_block_delta', { type: 'content_block_delta', index, delta });
      if (block.type === 'thinking') wire += event('content_block_delta', { type: 'content_block_delta', index, delta: { type: 'signature_delta', signature } });
      wire += event('content_block_stop', { type: 'content_block_stop', index });
    }
    return wire + event('message_delta', { type: 'message_delta', delta: { stop_reason: stop, stop_sequence: null }, usage: { output_tokens: 7 } }) + event('message_stop', { type: 'message_stop' });
  }
  if (family === 'openai.responses') {
    const output = tool ? [...(native ? [responseReasoning()] : []), responseCall()] : [responseMessage(text)];
    const resource = { id: 'bench-response', object: 'response', created_at: 1, model: 'bench-model', status: 'completed', output, usage, incomplete_details: null, error: null };
    if (!streaming) return JSON.stringify(resource);
    let sequence = 0;
    const frame = (type, fields) => event(type, { type, sequence_number: sequence++, ...fields });
    let wire = frame('response.created', { response: { ...resource, output: [], status: 'in_progress', usage: null } });
    for (const [output_index, item] of output.entries()) {
      const initial = item.type === 'message' ? { ...item, content: [], status: 'in_progress' } : item.type === 'reasoning' ? { id: item.id, type: item.type, summary: [] } : { ...item, arguments: '', status: 'in_progress' };
      wire += frame('response.output_item.added', { output_index, item: initial });
      if (item.type === 'function_call') {
        wire += frame('response.function_call_arguments.delta', { item_id: item.id, output_index, delta: item.arguments });
        wire += frame('response.function_call_arguments.done', { item_id: item.id, output_index, arguments: item.arguments });
      } else {
        const reasoning = item.type === 'reasoning';
        const prefix = reasoning ? 'response.reasoning_summary' : 'response.content';
        for (const [index, part] of (reasoning ? item.summary : item.content).entries()) {
          const owner = { item_id: item.id, output_index, [reasoning ? 'summary_index' : 'content_index']: index };
          wire += frame(`${prefix}_part.added`, { ...owner, part: { ...part, text: '' } });
          const stem = reasoning ? 'response.reasoning_summary_text' : 'response.output_text';
          wire += frame(`${stem}.delta`, { ...owner, delta: part.text });
          wire += frame(`${stem}.done`, { ...owner, text: part.text });
          wire += frame(`${prefix}_part.done`, { ...owner, part });
        }
      }
      wire += frame('response.output_item.done', { output_index, item });
    }
    return wire + frame('response.completed', { response: resource });
  }
  if (family === 'google.generate') {
    const parts = tool ? [...(native ? [geminiThought()] : []), geminiCall(native)] : [{ text }];
    const resource = parts_ => ({ responseId: 'bench-generation', modelVersion: 'bench-model', candidates: [{ index: 0, content: { role: 'model', parts: parts_ }, finishReason: 'STOP' }], usageMetadata: { promptTokenCount: 10, candidatesTokenCount: 7, totalTokenCount: 17 } });
    return streaming ? data(resource(parts)) : JSON.stringify(resource(parts));
  }
  if (family === 'google.interactions') {
    const steps = tool ? [...(native ? [interactionThought()] : []), interactionCall()] : [interactionText(text)];
    const resource = { id: 'bench-interaction', model: 'bench-model', status: tool ? 'requires_action' : 'completed', steps, usage: { total_input_tokens: 10, total_output_tokens: 7, total_tokens: 17 } };
    if (!streaming) return JSON.stringify(resource);
    const frame = (event_type, fields) => event(event_type, { event_type, ...fields });
    let wire = frame('interaction.created', { interaction: { id: resource.id, model: resource.model, status: 'in_progress' } });
    for (const [index, step] of steps.entries()) {
      const initial = step.type === 'thought' ? { ...step, summary: [], signature: '' } : step.type === 'function_call' ? { ...step, arguments: {} } : { ...step, content: [] };
      wire += frame('step.start', { index, step: initial });
      if (step.type === 'thought') {
        wire += frame('step.delta', { index, delta: { type: 'thought_summary', content: step.summary[0] } });
        wire += frame('step.delta', { index, delta: { type: 'thought_signature', signature } });
      } else if (step.type === 'function_call') wire += frame('step.delta', { index, delta: { type: 'arguments_delta', arguments: JSON.stringify(toolArgs) } });
      else wire += frame('step.delta', { index, delta: { type: 'text', text } });
      wire += frame('step.stop', { index });
    }
    return wire + frame('interaction.completed', { interaction: resource }) + 'event: done\ndata: [DONE]\n\n';
  }
  throw new Error('unsupported family');
}
function inspect(family, kind, body) {
  let history, result, call, native;
  if (family === 'openai.chat') {
    history = body.messages; result = history?.find(m => m.role === 'tool');
    call = history?.flatMap(m => m.tool_calls ?? []).find(c => c.id === callId);
    if (result && (result.tool_call_id !== callId || result.content !== JSON.stringify(toolArgs) || call?.function?.name !== toolName || !equal(JSON.parse(call.function.arguments), toolArgs))) throw new Error('chat ownership');
    native = history?.some(m => m.role === 'assistant' && equal(m.reasoning_details, chatReasoning()));
  } else if (family === 'anthropic.messages') {
    history = body.messages; const parts = history?.flatMap(m => m.content ?? []) ?? [];
    result = parts.find(p => p.type === 'tool_result'); call = parts.find(p => p.type === 'tool_use');
    if (result && (result.tool_use_id !== callId || result.content !== JSON.stringify(toolArgs) || !equal(call, messagesCall()))) throw new Error('messages ownership');
    native = parts.some(p => equal(p, messagesThinking()));
  } else if (family === 'openai.responses') {
    history = body.input; result = history?.find(p => p.type === 'function_call_output'); call = history?.find(p => p.type === 'function_call');
    if (result && (result.call_id !== callId || result.output !== JSON.stringify(toolArgs) || call?.name !== toolName || call.call_id !== callId || !equal(JSON.parse(call.arguments), toolArgs))) throw new Error('responses ownership');
    native = history?.some(p => equal(p, responseReasoning()));
  } else if (family === 'google.generate') {
    history = body.contents; const parts = history?.flatMap(m => m.parts ?? []) ?? [];
    result = parts.find(p => p.functionResponse)?.functionResponse; call = parts.find(p => p.functionCall)?.functionCall;
    if (result && (result.name !== toolName || !equal(result.response, toolArgs) || call?.name !== toolName || !equal(call.args, toolArgs))) throw new Error('gemini ownership');
    native = parts.some(p => equal(p, geminiThought())) && parts.some(p => equal(p, geminiCall(true)));
  } else {
    history = body.input; result = history?.find(p => p.type === 'function_result'); call = history?.find(p => p.type === 'function_call');
    if (result && (result.call_id !== callId || result.name !== toolName || result.result !== JSON.stringify(toolArgs) || !equal(call, interactionCall()))) throw new Error('interactions ownership');
    native = history?.some(p => equal(p, interactionThought()));
  }
  if (!Array.isArray(history) || !history.length) throw new Error('history');
  const user = history.find(item => item.role === 'user' || item.type === 'user_input');
  const userContent = user?.content ?? user?.parts;
  const prompt = typeof userContent === 'string' ? userContent
    : userContent?.filter(part => typeof part.text === 'string').map(part => part.text).join('');
  if (prompt !== 'benchmark-request:' + 'x'.repeat(config.payload_bytes)) throw new Error('workload prompt');
  const tokens = family === 'google.generate' ? body.generationConfig?.maxOutputTokens
    : family === 'google.interactions' ? body.generation_config?.max_output_tokens
    : body.max_output_tokens ?? body.max_completion_tokens ?? body.max_tokens;
  if (tokens !== config.output_tokens) throw new Error('workload output token cap');
  if (kind !== 'text') {
    const tools = family === 'google.generate' ? body.tools?.flatMap(tool => tool.functionDeclarations ?? []) : body.tools;
    if (!tools?.some(tool => (tool.function ?? tool).name === toolName)) throw new Error('workload tool declaration');
  }
  if (result && kind === 'native' && !native) throw new Error('native replay');
  return { continuation: Boolean(result), native: Boolean(result && kind === 'native' && native) };
}
const wireCache = new Map();
function cachedFixture(family, kind, continuation, streaming) {
  const key = `${family}|${kind}|${continuation}|${streaming}`;
  let entry = wireCache.get(key);
  if (!entry) {
    const wire = fixture(family, kind, continuation, streaming);
    const events = streaming && flushPerEvent ? wire.split('\n\n').filter(Boolean).map(frame => frame + '\n\n') : undefined;
    entry = { wire, events, length: Buffer.byteLength(wire) };
    wireCache.set(key, entry);
  }
  return entry;
}
function textOf(content) {
  return typeof content === 'string' ? content : Array.isArray(content) ? content.filter(part => typeof part.text === 'string').map(part => part.text).join('') : undefined;
}
// Request checks that apply when the matching option is configured.
function checkConfigured(family, req, body, streaming) {
  if (config.expect_streaming !== undefined && streaming !== config.expect_streaming) throw new Error('stream flag');
  if (config.api_key !== undefined) {
    const expected = family === 'anthropic.messages' ? req.headers['x-api-key'] === config.api_key
      : req.headers.authorization === 'Bearer ' + config.api_key;
    if (!expected) throw new Error('credential');
  }
  if (config.system_text !== undefined) {
    const system = family === 'anthropic.messages' ? textOf(body.system)
      : family === 'openai.chat' ? textOf(body.messages?.find(m => m.role === 'system' || m.role === 'developer')?.content) : undefined;
    if (system !== config.system_text) throw new Error('system prompt');
  }
}
async function handle(req, res) {
  req.on('error', () => {}); res.on('error', () => {});
  if (req.method === 'GET' && req.url === '/stats') { res.writeHead(200, { 'content-type': 'application/json' }); res.end(JSON.stringify(snapshot())); return; }
  const generateRoute = /^\/v1beta\/(models\/bench-model:(?:generateContent|streamGenerateContent\?alt=sse))$/.exec(req.url);
  const canonicalFamily = generateRoute ? 'google.generate' :
    req.url === '/v1beta/interactions' ? 'google.interactions' : undefined;
  const route = canonicalFamily === config.family && ['text', 'tool', 'native'].includes(config.case)
    ? ['', canonicalFamily.replace('.', '/'), config.case, generateRoute?.[1] ?? 'interactions']
    : /^\/(openai\/chat|anthropic\/messages|openai\/responses)\/(text|tool|native)\/(?:v1(?:beta)?\/)?(.+)$/.exec(req.url);
  const family = route?.[1].replace('/', '.'), kind = route?.[2];
  const cell = `${family}/${kind}`;
  const c = stats.cases[cell] ??= count();
  const bump = (key, n = 1) => { stats[key] += n; c[key] += n; };
  bump('requests'); bump(req.httpVersionMajor === 2 ? 'h2' : 'h1');
  let rawBody = '';
  try {
    if (!route || req.method !== 'POST') throw new Error('route');
    const endpoints = { 'openai.chat': ['chat/completions'], 'anthropic.messages': ['messages'],
      'openai.responses': ['responses'], 'google.interactions': ['interactions'],
      'google.generate': ['models/bench-model:generateContent', 'models/bench-model:streamGenerateContent?alt=sse'] };
    if (!endpoints[family]?.includes(route[3])) throw new Error('endpoint');
    const chunks = []; let bytes = 0;
    for await (const chunk of req) { bytes += chunk.length; bump('request_bytes', chunk.length); if (bytes > (1 << 24)) throw new Error('body bound'); chunks.push(chunk); }
    rawBody = Buffer.concat(chunks, bytes);
    const body = JSON.parse(rawBody);
    observe(stats.observed.user_agents, String(req.headers['user-agent'] ?? '').slice(0, 96));
    for (const key of Object.keys(body)) observe(stats.observed.body_keys, key);
    for (const name of Object.keys(req.headers)) observe(stats.observed.header_names, name);
    const model = family === 'google.generate' ? /models\/([^/:]+):/.exec(req.url)?.[1] : body.model;
    if (model !== 'bench-model') throw new Error('model');
    const checked = inspect(family, kind, body);
    if (checked.continuation) bump('continuations'); if (checked.native) bump('native_replays');
    const streaming = family === 'google.generate' ? req.url.includes(':streamGenerateContent') : body.stream === true;
    checkConfigured(family, req, body, streaming);
    const reply = cachedFixture(family, kind, checked.continuation, streaming);
    await barrier();
    if (config.server_delay_ms) await new Promise(resolve => setTimeout(resolve, config.server_delay_ms));
    if (res.destroyed) return;
    if (reply.events) {
      // One write per SSE event, so the body is sent with chunked transfer coding and arrives in pieces.
      res.writeHead(200, { 'content-type': 'text/event-stream', 'cache-control': 'no-cache' });
      for (const frame of reply.events) res.write(frame);
      bump('response_bytes', reply.length); res.end();
      return;
    }
    const headers = { 'content-type': streaming ? 'text/event-stream' : 'application/json', 'content-length': reply.length };
    res.writeHead(200, headers); bump('response_bytes', reply.length); res.end(reply.wire);
  } catch (error) {
    if (config.debug_rejections) process.stderr.write(`rejected ${req.method} ${req.url}: ${error.message}\n${String(rawBody).slice(0, 600)}\n`);
    bump('invalid'); if (!res.headersSent) res.writeHead(400, { 'content-type': 'application/json' }); res.end('{"error":{"message":"benchmark peer rejected request"}}');
  }
}
const workerCount = config.workers ?? 1, isWorker = cluster.isWorker;
function serve(dir, announce) {
  const credentials = { key: readFileSync(join(dir, 'server.key')), cert: readFileSync(join(dir, 'server.pem')) };
  // node:http2 always advertises h2, so an http/1.1-only peer is a plain node:https server.
  server = config.alpn === 'h1' ? https.createServer(credentials, handle) : http2.createSecureServer({ ...credentials, allowHTTP1: true }, handle);
  // A benchmark connection must stay open between a warm-up and a measured window.
  server.keepAliveTimeout = 0; server.headersTimeout = 0; server.requestTimeout = 0;
  server.on('connection', socket => { ++stats.connections; sockets.add(socket); socket.on('error', () => {}); socket.once('close', () => sockets.delete(socket)); });
  server.on('session', session => { sessions.add(session); session.on('error', () => {}); session.once('close', () => sessions.delete(session)); });
  server.on('tlsClientError', () => { ++stats.tls_errors; });
  server.on('error', () => shutdown(1));
  server.listen(0, '127.0.0.1', () => announce(server.address().port));
}
function sum(into, from) {
  for (const [key, value] of Object.entries(from)) {
    if (typeof value === 'number') into[key] = (into[key] ?? 0) + value;
    else if (value && typeof value === 'object') into[key] = sum(into[key] ?? {}, value);
  }
  return into;
}
function control(onStats, onShutdown) {
  const lines = readline.createInterface({ input: process.stdin });
  lines.on('line', line => { try { const command = JSON.parse(line); if (command.stats === true) onStats(); else if (command.shutdown === true) onShutdown(); else reply({ error: 'unsupported control command' }); } catch { reply({ error: 'invalid control command' }); } });
  lines.on('close', () => onShutdown());
}
// Several serving processes: the primary owns the certificates, the control channel, the barrier count and the merged statistics.
function primary() {
  let listening = 0, holds = 0, released = false, barrierTimer, nextId = 0, barrierTimeouts = 0;
  const pending = new Map();
  const workers = () => Object.values(cluster.workers);
  const release = () => { released = true; clearTimeout(barrierTimer); for (const worker of workers()) worker.send({ type: 'release' }); };
  for (let i = 0; i < workerCount; i++) cluster.fork({ SDKB_PEER_DIR: directory });
  cluster.on('exit', () => { if (!stopping) shutdown(1); });
  cluster.on('message', (worker, message) => {
    if (message.type === 'listening') {
      if (++listening === workerCount) reply({ ready: true, version: 1, port: message.port, host: '127.0.0.1', ca_file: join(directory, 'ca.pem') });
    } else if (message.type === 'hold') {
      if (released) return;
      if (++holds >= config.warm_barrier) release();
      else barrierTimer ??= setTimeout(() => { ++barrierTimeouts; release(); }, 15000);
    } else if (message.type === 'stats') {
      const entry = pending.get(message.id);
      entry.parts.push(message.stats);
      if (entry.parts.length < workerCount) return;
      pending.delete(message.id);
      const merged = entry.parts.reduce((acc, part) => sum(acc, part), {});
      const own = process.cpuUsage();
      merged.version = 1; merged.cpu_user_us += own.user; merged.cpu_system_us += own.system; merged.barrier_timeouts += barrierTimeouts;
      reply(merged);
    }
  });
  control(() => { const id = ++nextId; pending.set(id, { parts: [] }); for (const worker of workers()) worker.send({ type: 'stats', id }); },
    () => { for (const worker of workers()) worker.send({ type: 'shutdown' }); shutdown(); });
}
if (isWorker) {
  process.on('message', message => {
    if (message.type === 'stats') process.send({ type: 'stats', id: message.id, stats: snapshot() });
    else if (message.type === 'release') openBarrier();
    else if (message.type === 'shutdown') shutdown();
  });
  serve(process.env.SDKB_PEER_DIR, port => process.send({ type: 'listening', port }));
} else try {
  directory = mkdtempSync('/tmp/schemaprovider-benchmark-');
  openssl('req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-sha256', '-keyout', 'ca.key', '-out', 'ca.pem', '-days', '2', '-subj', '/CN=Benchmark ephemeral CA', '-addext', 'basicConstraints=critical,CA:TRUE', '-addext', 'keyUsage=critical,keyCertSign,cRLSign');
  openssl('req', '-new', '-newkey', 'rsa:2048', '-nodes', '-sha256', '-keyout', 'server.key', '-out', 'server.csr', '-subj', '/CN=localhost');
  writeFileSync(join(directory, 'server.ext'), 'basicConstraints=critical,CA:FALSE\nkeyUsage=critical,digitalSignature,keyEncipherment\nextendedKeyUsage=serverAuth\nsubjectAltName=DNS:localhost,IP:127.0.0.1\n', { mode: 0o600 });
  openssl('x509', '-req', '-in', 'server.csr', '-CA', 'ca.pem', '-CAkey', 'ca.key', '-set_serial', '1', '-out', 'server.pem', '-days', '2', '-sha256', '-extfile', 'server.ext');
  if (workerCount > 1) primary();
  else {
    serve(directory, port => reply({ ready: true, version: 1, port, host: '127.0.0.1', ca_file: join(directory, 'ca.pem') }));
    control(() => reply(snapshot()), () => shutdown());
  }
} catch { process.stderr.write('benchmark peer startup failed\n'); shutdown(1); }
