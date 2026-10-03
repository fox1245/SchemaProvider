// Independent model-free TLS protocol oracle; no production encoder imports.
import http2 from 'node:http2';
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
const text = 'benchmark-ok:' + 'x'.repeat(config.payload_bytes);
const toolArgs = { value: 'bench' }, toolName = 'bench_echo', callId = 'bench-call';
const signature = 'BENCHMARK_PUBLIC_SYNTHETIC_SIGNATURE';
// Public synthetic encrypted carrier: replay fidelity only, not vendor-valid cryptography.
const chatReasoning = () => [{ id: 'bench-reasoning', type: 'reasoning.encrypted',
  data: signature, format: 'benchmark.synthetic', index: 0 }];
const count = () => ({ requests: 0, h1: 0, h2: 0, invalid: 0, continuations: 0,
  native_replays: 0, request_bytes: 0, response_bytes: 0 });
const stats = { version: 1, ...count(), connections: 0, active_connections: 0, tls_errors: 0, cases: {} };
const sockets = new Set(), sessions = new Set();
let directory, server, stopping = false;
const reply = value => process.stdout.write(JSON.stringify(value) + '\n');
const snapshot = () => ({ ...stats, active_connections: sockets.size });
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
  try {
    if (!route || req.method !== 'POST') throw new Error('route');
    const endpoints = { 'openai.chat': ['chat/completions'], 'anthropic.messages': ['messages'],
      'openai.responses': ['responses'], 'google.interactions': ['interactions'],
      'google.generate': ['models/bench-model:generateContent', 'models/bench-model:streamGenerateContent?alt=sse'] };
    if (!endpoints[family]?.includes(route[3])) throw new Error('endpoint');
    const chunks = []; let bytes = 0;
    for await (const chunk of req) { bytes += chunk.length; bump('request_bytes', chunk.length); if (bytes > (1 << 24)) throw new Error('body bound'); chunks.push(chunk); }
    const body = JSON.parse(Buffer.concat(chunks, bytes));
    const model = family === 'google.generate' ? /models\/([^/:]+):/.exec(req.url)?.[1] : body.model;
    if (model !== 'bench-model') throw new Error('model');
    const checked = inspect(family, kind, body);
    if (checked.continuation) bump('continuations'); if (checked.native) bump('native_replays');
    const streaming = family === 'google.generate' ? req.url.includes(':streamGenerateContent') : body.stream === true;
    const wire = fixture(family, kind, checked.continuation, streaming);
    if (config.server_delay_ms) await new Promise(resolve => setTimeout(resolve, config.server_delay_ms));
    if (res.destroyed) return;
    const headers = { 'content-type': streaming ? 'text/event-stream' : 'application/json', 'content-length': Buffer.byteLength(wire) };
    res.writeHead(200, headers); bump('response_bytes', Buffer.byteLength(wire)); res.end(wire);
  } catch {
    bump('invalid'); if (!res.headersSent) res.writeHead(400, { 'content-type': 'application/json' }); res.end('{"error":{"message":"benchmark peer rejected request"}}');
  }
}
try {
  directory = mkdtempSync('/tmp/schemaprovider-benchmark-');
  openssl('req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-sha256', '-keyout', 'ca.key', '-out', 'ca.pem', '-days', '2', '-subj', '/CN=Benchmark ephemeral CA', '-addext', 'basicConstraints=critical,CA:TRUE', '-addext', 'keyUsage=critical,keyCertSign,cRLSign');
  openssl('req', '-new', '-newkey', 'rsa:2048', '-nodes', '-sha256', '-keyout', 'server.key', '-out', 'server.csr', '-subj', '/CN=localhost');
  writeFileSync(join(directory, 'server.ext'), 'basicConstraints=critical,CA:FALSE\nkeyUsage=critical,digitalSignature,keyEncipherment\nextendedKeyUsage=serverAuth\nsubjectAltName=DNS:localhost,IP:127.0.0.1\n', { mode: 0o600 });
  openssl('x509', '-req', '-in', 'server.csr', '-CA', 'ca.pem', '-CAkey', 'ca.key', '-set_serial', '1', '-out', 'server.pem', '-days', '2', '-sha256', '-extfile', 'server.ext');
  server = http2.createSecureServer({ key: readFileSync(join(directory, 'server.key')), cert: readFileSync(join(directory, 'server.pem')), allowHTTP1: true }, handle);
  server.on('connection', socket => { ++stats.connections; sockets.add(socket); socket.on('error', () => {}); socket.once('close', () => sockets.delete(socket)); });
  server.on('session', session => { sessions.add(session); session.on('error', () => {}); session.once('close', () => sessions.delete(session)); });
  server.on('tlsClientError', () => { ++stats.tls_errors; });
  server.on('error', () => shutdown(1));
  server.listen(0, '127.0.0.1', () => reply({ ready: true, version: 1, port: server.address().port, host: '127.0.0.1', ca_file: join(directory, 'ca.pem') }));
  const control = readline.createInterface({ input: process.stdin });
  control.on('line', line => { try { const command = JSON.parse(line); if (command.stats === true) reply(snapshot()); else if (command.shutdown === true) shutdown(); else reply({ error: 'unsupported control command' }); } catch { reply({ error: 'invalid control command' }); } });
  control.on('close', () => shutdown());
} catch { process.stderr.write('benchmark peer startup failed\n'); shutdown(1); }
