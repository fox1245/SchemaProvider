import http from 'node:http';
import readline from 'node:readline';
import { inflateSync } from 'node:zlib';
import { createHash } from 'node:crypto';
import { isDeepStrictEqual } from 'node:util';

const key = 'INTERACTIONS_SYNTHETIC_KEY';
const signature = 'INTERACTIONS_FINAL_PRIVATE_SIGNATURE';
const cases = new Map(), sockets = new Set(), waiters = new Set();
let unexpected = 0;
const reply = value => process.stdout.write(`${JSON.stringify(value)}\n`);
const notify = () => { for (const wake of [...waiters]) wake(); };
const snapshot = c => ({ count: c.count, invalid: c.invalid, unexpected, faults: c.faults,
  held: c.held.size, closed: c.closed, bytes: c.bytes, replayed: c.replayed, image_checked: c.image_checked });
// The peer inspects actual PNG pixels independently of encoder and scene labels.
function observe(image) {
  if (image.type !== 'image' || image.mime_type !== 'image/png' || typeof image.data !== 'string') throw new Error('image');
  const bytes = Buffer.from(image.data, 'base64');
  if (bytes.toString('base64') !== image.data || !bytes.subarray(0, 8).equals(Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]))) throw new Error('PNG');
  let width, height, chunks = [];
  for (let at = 8; at + 12 <= bytes.length;) {
    const size = bytes.readUInt32BE(at), type = bytes.toString('ascii', at + 4, at + 8);
    const data = bytes.subarray(at + 8, at + 8 + size);
    if (at + 12 + size > bytes.length) throw new Error('chunk');
    if (type === 'IHDR') { width = data.readUInt32BE(0); height = data.readUInt32BE(4); if (data[8] !== 8 || data[9] !== 2) throw new Error('format'); }
    if (type === 'IDAT') chunks.push(data);
    at += 12 + size;
  }
  if (width !== 640 || height !== 480) throw new Error('dimensions');
  const pixels = inflateSync(Buffer.concat(chunks), { maxOutputLength: height * (width * 3 + 1) });
  if (pixels.length !== height * (width * 3 + 1)) throw new Error('pixels');
  const colors = new Uint8Array(width * height);
  for (let y = 0; y < height; ++y) {
    if (pixels[y * (width * 3 + 1)] !== 0) throw new Error('filter');
    for (let x = 0; x < width; ++x) {
      const at = y * (width * 3 + 1) + 1 + x * 3, r = pixels[at], g = pixels[at + 1], b = pixels[at + 2];
      colors[y * width + x] = r > 180 && g < 80 && b < 80 ? 1 : b > 180 && r < 80 && g < 100 ? 2 : 0;
    }
  }
  let red_circles = 0, blue_squares = 0;
  const seen = new Uint8Array(colors.length);
  for (let p = 0; p < colors.length; ++p) {
    if (!colors[p] || seen[p]) continue;
    const color = colors[p]; if (color === 1) ++red_circles; else ++blue_squares;
    const stack = [p]; seen[p] = 1;
    while (stack.length) {
      const at = stack.pop(), x = at % width;
      for (const next of [x ? at - 1 : -1, x + 1 < width ? at + 1 : -1, at - width, at + width]) {
        if (next < 0 || next >= colors.length || seen[next] || colors[next] !== color) continue;
        seen[next] = 1; stack.push(next);
      }
    }
  }
  return { answer: { red_circles, blue_squares, weighted: 2 * red_circles + blue_squares },
    hash: createHash('sha256').update(bytes).digest('hex') };
}
const usage = { total_input_tokens: 100, total_output_tokens: 25, total_thought_tokens: 22,
  total_tokens: 147, total_cached_tokens: 4, total_tool_use_tokens: 50 };
const thought = () => ({ type: 'thought', signature, summary: [{ type: 'text', text: 'Count colored components' }], future: { retained: true } });
const call = c => ({ type: 'function_call', id: 'scene_call', name: 'observe_scene', arguments: c.answer });
const output = text => ({ type: 'model_output', content: [{ type: 'text', text, annotations: [] }] });
const grouped = c => [thought(), call(c), output('need host result')];
const resource = (model, steps, status = 'completed', counters = usage) => ({ id: 'interaction_owned', model, status, steps, usage: counters });
const event = (event_type, fields) => `event: ${event_type}\ndata: ${JSON.stringify({ event_type, ...fields })}\n\n`;
function frames(model, steps, status, counters) {
  let wire = event('interaction.created', { interaction: { id: 'interaction_owned', model, status: 'in_progress' } });
  // Start all steps before their deltas to exercise real index interleaving.
  for (const [index, step] of steps.entries()) {
    const initial = step.type === 'thought' ? { ...step, summary: [], signature: '' }
      : step.type === 'function_call' ? { ...step, arguments: {} }
      : { ...step, content: [] };
    wire += event('step.start', { index, step: initial });
  }
  for (const [index, step] of steps.entries()) {
    if (step.type === 'thought') {
      for (const content of step.summary) wire += event('step.delta', { index, delta: { type: 'thought_summary', content } });
      wire += event('step.delta', { index, delta: { type: 'thought_signature', signature: 'INTERACTIONS_STREAM_PRIVATE_SIGNATURE' } });
    } else if (step.type === 'function_call') {
      const args = JSON.stringify(step.arguments), midpoint = Math.floor(args.length / 2);
      for (const arguments_ of [args.slice(0, midpoint), args.slice(midpoint)]) wire += event('step.delta', { index, delta: { type: 'arguments_delta', arguments: arguments_ } });
    } else if (step.type === 'model_output') for (const content of step.content) wire += event('step.delta', { index, delta: { type: 'text', text: content.text } });
    wire += event('step.stop', { index });
  }
  wire += event('interaction.completed', { interaction: resource(model, steps, status, counters) });
  return wire;
}
function prefix(model) {
  return event('interaction.created', { interaction: { id: 'interaction_owned', model, status: 'in_progress' } })
    + event('step.start', { index: 0, step: { type: 'model_output', content: [] } })
    + event('step.delta', { index: 0, delta: { type: 'text', text: 'partial owned' } });
}
function valid(req, body) {
  return req.method === 'POST' && req.url === '/v1beta/interactions' && req.headers['x-goog-api-key'] === key && !req.headers.authorization
    && typeof body.stream === 'boolean' && body.store === false && body.service_tier === 'standard'
    && body.system_instruction === 'Count the actual image' && body.generation_config?.max_output_tokens === 128
    && body.generation_config.thinking_level === 'low' && body.generation_config.thinking_summaries === 'auto'
    && body.tools?.length === 1 && body.tools[0].type === 'function' && body.tools[0].name === 'observe_scene'
    && body.tools[0].parameters?.type === 'object' && Array.isArray(body.input)
    && !['agent', 'environment', 'background', 'previous_interaction_id', 'conversation'].some(k => k in body);
}
const server = http.createServer(async (req, res) => {
  let c;
  try {
    const chunks = []; let bytes = 0;
    for await (const chunk of req) { bytes += chunk.length; if (bytes > (1 << 20)) throw new Error('bound'); chunks.push(chunk); }
    const body = JSON.parse(Buffer.concat(chunks)); c = cases.get(body.model);
    if (!c) { ++unexpected; res.writeHead(400); res.end(); return; }
    ++c.count;
    if (!valid(req, body)) ++c.invalid;
    const user = body.input[0];
    if (user?.type !== 'user_input' || user.content?.length !== 3 || user.content[0].type !== 'text' ||
        user.content[0].text !== 'Count red circles and blue squares' || user.content[2].type !== 'text' || user.content[2].text !== 'Use the host function') throw new Error('order');
    const observed = observe(user.content[1]); ++c.image_checked;
    if (c.count === 1) { c.answer = observed.answer; c.hash = observed.hash; c.user = user; }
    if (c.count === 2) {
      const expected = [c.user, ...grouped(c), { type: 'function_result', call_id: 'scene_call', name: 'observe_scene', result: JSON.stringify(c.answer), is_error: false }];
      if (!isDeepStrictEqual(body.input, expected) || c.hash !== observed.hash) ++c.invalid; else ++c.replayed;
    }
    if (c.scenario === 'forced' && c.count === 1 && !isDeepStrictEqual(body.generation_config.tool_choice, { allowed_tools: { mode: 'any', tools: ['observe_scene'] } })) ++c.invalid;
    res.on('close', () => { ++c.closed; c.held.delete(res); notify(); }); notify();
    const fault = () => { ++c.faults; notify(); };
    if (c.scenario === 'reset') { fault(); res.socket.resetAndDestroy(); return; }
    let status = c.count === 1 && ['grouped', 'forced'].includes(c.scenario) ? 'requires_action' : 'completed';
    let steps = status === 'requires_action' ? grouped(c) : [thought(), output(JSON.stringify(c.answer))];
    let counters = usage;
    if (c.scenario === 'incomplete') status = 'incomplete';
    if (c.scenario === 'failed') status = 'failed';
    if (c.scenario === 'cancelled') status = 'cancelled';
    if (c.scenario === 'nullable') counters = { total_input_tokens: 100, total_output_tokens: 25, total_thought_tokens: null, total_tokens: 125 };
    if (c.scenario === 'unsupported') steps = [{ type: 'google_search_call', id: 'server_tool' }];
    const data = body.stream ? frames(body.model, steps, status, counters) : JSON.stringify(resource(body.model, steps, status, counters));
    const headers = { 'Content-Type': body.stream ? 'text/event-stream' : 'application/json', Connection: 'close' };
    if (c.scenario === 'short-close') headers['Content-Length'] = Buffer.byteLength(data) + 17;
    res.writeHead(200, headers); res.flushHeaders();
    const send = value => { c.bytes += Buffer.byteLength(value); res.write(value); };
    if (c.scenario === 'hold') { c.held.set(res, () => { send(data); res.end(); }); notify(); return; }
    if (c.scenario === 'partial' || c.scenario === 'partial-error') {
      send(prefix(body.model));
      if (c.scenario === 'partial-error') { send(event('error', { error: { message: signature + key } })); fault(); res.end(); }
      else { c.held.set(res, () => { fault(); res.destroy(); }); notify(); }
      return;
    }
    if (c.scenario === 'status-only') {
      send(event('interaction.created', { interaction: { id: 'interaction_owned', model: body.model, status: 'in_progress' } }));
      send(event('interaction.status_update', { interaction_id: 'interaction_owned', status: 'completed' })); fault(); res.end(); return;
    }
    send(data);
    if (c.scenario === 'terminal-error') { send(event('error', { error: { message: signature + key } })); fault(); res.end(); return; }
    if (c.scenario === 'close-gate' || c.scenario === 'terminal-reset') {
      c.held.set(res, () => { if (c.scenario === 'terminal-reset') { fault(); res.destroy(); } else res.end(); }); notify(); return;
    }
    if (c.scenario === 'short-close') { fault(); res.socket.end(); return; }
    if (body.stream) send('event: done\ndata: [DONE]\n\n');
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
      cases.set(command.arm, { scenario: command.scenario, count: 0, invalid: 0, faults: 0,
        held: new Map(), closed: 0, bytes: 0, replayed: 0, image_checked: 0 }); reply({ armed: true }); return;
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
  } catch { reply({ error: 'Interactions peer control failure' }); }
});
control.on('close', () => { server.close(); for (const socket of sockets) socket.destroy(); });
