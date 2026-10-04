// Model-free HTTP oracle: synthetic sealed items prove local replay, not vendor acceptance.
import http from 'node:http';
import readline from 'node:readline';
import { isDeepStrictEqual } from 'node:util';

const key = 'RESPONSES_SECRET_MARKER_790ec1';
const cipher = 'RESPONSES_CIPHER_MARKER_274ab2';
const cases = new Map();
const sockets = new Set();
const waiters = new Set();
let unexpected = 0;
const reply = value => process.stdout.write(`${JSON.stringify(value)}\n`);
const notify = () => { for (const wake of [...waiters]) wake(); };
const snapshot = c => ({ count: c.count, invalid: c.invalid, unexpected, faults: c.faults,
  held: c.held.size, closed: c.closed, bytes: c.bytes, replayed: c.replayed,
  stored: c.history.size, continued: c.continued });
const reasoning = () => ({ id: 'rs_native', type: 'reasoning', status: 'completed',
  summary: [{ type: 'summary_text', text: 'Use the lookup result' }], encrypted_content: cipher });
const call = () => ({ id: 'fc_item', type: 'function_call', status: 'completed',
  call_id: 'call_owned', name: 'lookup', arguments: '{"x":1}' });
const message = text => ({ id: 'msg_native', type: 'message', status: 'completed', role: 'assistant',
  phase: 'final_answer', content: [{ type: 'output_text', text, annotations: [] }] });
const grouped = () => [reasoning(), call(), message('need lookup')];
const usage = { input_tokens: 10, output_tokens: 7, total_tokens: 17,
  input_tokens_details: { cached_tokens: 4 }, output_tokens_details: { reasoning_tokens: 3 } };
function response(model, output, status = 'completed', counters = usage, id = 'resp_native') {
  return { id, object: 'response', created_at: 1, model, status, output, usage: counters,
    incomplete_details: status === 'incomplete' ? { reason: 'max_output_tokens' } : null, error: null };
}
function frames(model, output, status = 'completed', counters = usage, id = 'resp_native') {
  let sequence = 0;
  const frame = (type, fields) => `event: ${type}\ndata: ${JSON.stringify({ type, sequence_number: sequence++, ...fields })}\n\n`;
  let wire = frame('response.created', { response: response(model, [], 'in_progress', null, id) });
  for (const [output_index, item] of output.entries()) {
    const initial = item.type === 'message' ? { ...item, status: 'in_progress', content: [] }
      : item.type === 'reasoning' ? { id: item.id, type: item.type, summary: [] }
      : item.type === 'function_call' ? { ...item, status: 'in_progress', arguments: '' } : item;
    wire += frame('response.output_item.added', { output_index, item: initial });
    if (item.type === 'message' || item.type === 'reasoning') {
      const summary = item.type === 'reasoning';
      const parts = summary ? item.summary : item.content;
      for (const [index, part] of parts.entries()) {
        const owner = { item_id: item.id, output_index, [summary ? 'summary_index' : 'content_index']: index };
        const prefix = summary ? 'response.reasoning_summary' : 'response.content';
        wire += frame(`${prefix}_part.added`, { ...owner, part: { ...part, text: '' } });
        const textType = summary ? 'response.reasoning_summary_text' : 'response.output_text';
        wire += frame(`${textType}.delta`, { ...owner, delta: part.text });
        wire += frame(`${textType}.done`, { ...owner, text: part.text });
        wire += frame(`${prefix}_part.done`, { ...owner, part });
      }
    } else if (item.type === 'function_call') {
      const owner = { item_id: item.id, output_index };
      wire += frame('response.function_call_arguments.delta', { ...owner, delta: item.arguments });
      wire += frame('response.function_call_arguments.done', { ...owner, arguments: item.arguments });
    }
    wire += frame('response.output_item.done', { output_index, item });
  }
  // The live API may supply a different terminal encrypted representation.
  // Replay must use the completed output_item.done bytes, not this envelope.
  const aggregate = structuredClone(output);
  for (const item of aggregate) if (item.type === 'reasoning') item.encrypted_content = 'TERMINAL_ONLY_REPRESENTATION';
  wire += frame(status === 'incomplete' ? 'response.incomplete' : 'response.completed',
    { response: response(model, aggregate, status, counters, id) });
  return wire;
}
function prefix(model) {
  const event = (type, fields) => `event: ${type}\ndata: ${JSON.stringify({ type, ...fields })}\n\n`;
  return event('response.created', { response: response(model, [], 'in_progress', null) })
    + event('response.output_item.added', { output_index: 0, item: { id: 'msg_native', type: 'message', role: 'assistant', status: 'in_progress', content: [] } })
    + event('response.content_part.added', { item_id: 'msg_native', output_index: 0, content_index: 0, part: { type: 'output_text', text: '', annotations: [] } })
    + event('response.output_text.delta', { item_id: 'msg_native', output_index: 0, content_index: 0, delta: 'partial owned' });
}
function valid(req, body, stateful = false) {
  return req.method === 'POST' && req.url === '/v1/responses' && req.headers.authorization === `Bearer ${key}`
    && typeof body.stream === 'boolean' && Array.isArray(body.input) && body.store === stateful
    && (stateful || isDeepStrictEqual(body.include, ['reasoning.encrypted_content'])) && body.max_output_tokens === 128
    && body.instructions === 'Answer briefly' && isDeepStrictEqual(body.reasoning, { effort: 'low', summary: 'auto' })
    && body.tools?.length === 1 && body.tools[0].type === 'function' && body.tools[0].name === 'lookup'
    && body.tools[0].description === 'Find a value' && body.tools[0].strict === true
    && isDeepStrictEqual(body.tools[0].parameters, { type: 'object', properties: { x: { type: 'integer' } }, required: ['x'], additionalProperties: false })
    && (stateful || !('previous_response_id' in body)) && !('conversation' in body) && !('background' in body) && !('service_tier' in body);
}
function replay(body) {
  return body.input.length === 5 && grouped().every((item, index) => isDeepStrictEqual(body.input[index + 1], item))
    && isDeepStrictEqual(body.input[4], { type: 'function_call_output', call_id: 'call_owned', output: 'one' });
}
function stateful(req, res, body, c) {
  if (!valid(req, body, true) || body.parallel_tool_calls !== false ||
      body.text?.verbosity !== 'high' || body.truncation !== 'auto' ||
      !isDeepStrictEqual(body.include, [])) ++c.invalid;
  const previous = body.previous_response_id;
  const prior = previous === undefined ? [] : c.history.get(previous);
  if (!prior) {
    ++c.faults;
    res.writeHead(404, { 'Content-Type': 'application/json', Connection: 'close' });
    res.end(JSON.stringify({ error: { type: 'invalid_request_error', code: 'response_not_found', message: 'Unknown response cursor' } }));
    return;
  }
  let output;
  if (c.scenario === 'stateful-text') {
    const expected = c.history.size === 0 ? 'Remember blue' : c.history.size === 1 ? 'What color?' : 'What did I ask?';
    if (!isDeepStrictEqual(body.input, [{ role: 'user', content: [{ type: 'input_text', text: expected }] }])) ++c.invalid;
    const remembered = prior.find(item => item.role === 'user')?.content[0].text;
    const question = [...prior].reverse().find(item => item.role === 'user')?.content[0].text;
    output = [message(!previous ? 'remembered' : expected === 'What color?' ? remembered?.slice(9) : question)];
  } else {
    if (!previous) {
      if (!isDeepStrictEqual(body.input, [{ role: 'user', content: [{ type: 'input_text', text: 'synthetic question' }] }])) ++c.invalid;
      output = [call()];
    } else {
      const pending = [...prior].reverse().find(item => item.type === 'function_call');
      const expected = { type: 'function_call_output', call_id: pending?.call_id, output: pending?.call_id === 'call_owned' ? 'one' : 'two' };
      if (!isDeepStrictEqual(body.input, [expected])) ++c.invalid;
      output = pending?.call_id === 'call_owned'
        ? [{ ...call(), id: 'fc_next', call_id: 'call_next', arguments: '{"x":2}' }]
        : [message(prior.filter(item => item.type === 'function_call_output').map(item => item.output).concat(body.input[0].output).join('+'))];
    }
  }
  if (previous) ++c.continued;
  const id = `resp_state_${c.history.size + 1}`;
  c.history.set(id, structuredClone([...prior, ...body.input, ...output]));
  const data = body.stream ? frames(body.model, output, 'completed', usage, id)
    : JSON.stringify(response(body.model, output, 'completed', usage, id));
  c.bytes += Buffer.byteLength(data);
  res.writeHead(200, { 'Content-Type': body.stream ? 'text/event-stream' : 'application/json', Connection: 'close' });
  res.end(data);
}
const server = http.createServer(async (req, res) => {
  let c;
  try {
    const chunks = []; let bytes = 0;
    for await (const chunk of req) { bytes += chunk.length; if (bytes > (1 << 20)) throw new Error('bound'); chunks.push(chunk); }
    const body = JSON.parse(Buffer.concat(chunks).toString('utf8'));
    c = cases.get(body.model);
    if (!c) { ++unexpected; res.writeHead(400); res.end(); return; }
    ++c.count;
    if (c.scenario.startsWith('stateful-')) {
      res.on('close', () => { ++c.closed; notify(); });
      stateful(req, res, body, c); notify(); return;
    }
    if (!valid(req, body)) ++c.invalid;
    if (c.scenario === 'forced' && !isDeepStrictEqual(body.tool_choice, { type: 'function', name: 'lookup' })) ++c.invalid;
    if (c.count === 1) c.prefix = body.input[0];
    if (c.count === 2 && (c.scenario === 'grouped' || c.scenario === 'replay-rejected')) {
      if (!replay(body) || !isDeepStrictEqual(body.input[0], c.prefix)) ++c.invalid;
      else ++c.replayed;
    }
    res.on('close', () => { ++c.closed; c.held.delete(res); notify(); });
    notify();
    const fault = () => { ++c.faults; notify(); };
    if (c.scenario === 'reset') { fault(); res.socket.resetAndDestroy(); return; }
    if (c.scenario === 'quota') {
      fault(); res.writeHead(429, { 'Content-Type': 'application/json', Connection: 'close' });
      res.end(JSON.stringify({ error: { type: 'insufficient_quota', code: 'insufficient_quota', message: key + cipher } })); return;
    }
    if (c.scenario === 'replay-rejected' && c.count === 2) {
      fault(); res.writeHead(400, { 'Content-Type': 'application/json', Connection: 'close' });
      res.end(JSON.stringify({ error: { type: 'invalid_request_error', code: 'invalid_encrypted_content', message: key + cipher } })); return;
    }
    const status = c.scenario === 'incomplete' ? 'incomplete' : 'completed';
    const output = c.scenario === 'grouped' || c.scenario === 'replay-rejected' || c.scenario === 'forced'
      ? (c.count === 1 ? grouped() : [reasoning(), message('answer one')])
      : c.scenario === 'incomplete' ? [reasoning(), { ...call(), status: 'incomplete', arguments: '{"x":' }]
      : c.scenario === 'opaque' ? [{ id: 'ws_1', type: 'web_search_call', status: 'completed', action: { type: 'search', query: 'x' } }, message('hello')]
      : [reasoning(), message(c.text ?? 'hello')];
    const data = body.stream ? frames(body.model, output, status) : JSON.stringify(response(body.model, output, status));
    const headers = { 'Content-Type': body.stream ? 'text/event-stream' : 'application/json', Connection: 'close' };
    if (c.scenario === 'short-close') headers['Content-Length'] = Buffer.byteLength(data) + 19;
    res.writeHead(200, headers); res.flushHeaders();
    const send = value => { c.bytes += Buffer.byteLength(value); res.write(value); };
    if (c.scenario.startsWith('named-')) {
      if (c.scenario.startsWith('named-completed-')) send(data);
      else if (c.scenario.startsWith('named-partial-')) send(prefix(body.model));
      const error = { code: 'rate_limit_exceeded', message: key };
      const payload = c.scenario.endsWith('-malformed') ? '{'
        : c.scenario.endsWith('-sentinel') ? '[DONE]'
        : JSON.stringify({ ...(c.scenario.endsWith('-wrong-type') ? { type: 'response.completed' } : {}), error,
            vendor: { b: [2, 1], a: true } });
      send(`event: error\ndata: ${payload}\n\ndata: [DONE]\n\n`);
      fault(); res.end(); return;
    }
    if (c.scenario === 'hold') { c.held.set(res, () => { send(data); res.end(); }); notify(); return; }
    if (c.scenario === 'partial' || c.scenario === 'partial-error') {
      send(prefix(body.model));
      if (c.scenario === 'partial-error') {
        send(`event: error\ndata: ${JSON.stringify({ type: 'error', code: 'server_error', message: key })}\n\n`); fault(); res.end();
      } else { c.held.set(res, () => { fault(); res.destroy(); }); notify(); }
      return;
    }
    send(data);
    if (c.scenario === 'completed-error') { send(`event: error\ndata: ${JSON.stringify({ type: 'error', code: 'server_error', message: key + cipher })}\n\n`); fault(); res.end(); return; }
    if (c.scenario === 'close-gate' || c.scenario === 'completed-reset') {
      c.held.set(res, () => { if (c.scenario === 'completed-reset') { fault(); res.destroy(); } else res.end(); }); notify(); return;
    }
    if (c.scenario === 'short-close') { fault(); res.socket.end(); return; }
    res.end();
  } catch { if (c) ++c.invalid; else ++unexpected; res.destroy(); notify(); }
});
server.on('connection', socket => { sockets.add(socket); socket.on('close', () => sockets.delete(socket)); });
server.on('clientError', (_error, socket) => socket.destroy());
server.listen(0, '127.0.0.1', () => reply({ port: server.address().port }));
const control = readline.createInterface({ input: process.stdin });
control.on('line', async line => {
  try {
    const command = JSON.parse(line);
    if (command.arm) {
      if (cases.has(command.arm)) throw new Error('duplicate');
      cases.set(command.arm, { scenario: command.scenario, text: command.text, count: 0, invalid: 0,
        faults: 0, held: new Map(), closed: 0, bytes: 0, replayed: 0, history: new Map(), continued: 0 }); reply({ armed: true }); return;
    }
    const c = cases.get(command.model); if (!c) throw new Error('unknown');
    if (command.wait) await new Promise((resolve, reject) => {
      const ready = () => { const stats = snapshot(c); if (Object.entries(command.wait).every(([key, value]) => stats[key] >= value)) {
        clearTimeout(timer); waiters.delete(ready); resolve(); } };
      const timer = setTimeout(() => { waiters.delete(ready); reject(new Error('timeout')); }, 12000);
      waiters.add(ready); ready();
    });
    if (command.release) { const held = [...c.held.values()]; c.held.clear(); for (const release of held) release(); notify(); }
    reply(snapshot(c));
  } catch { reply({ error: 'Responses peer control failure' }); }
});
control.on('close', () => { server.close(); for (const socket of sockets) socket.destroy(); });
