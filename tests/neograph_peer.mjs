// Transparent model-free observation proxy over the existing runtime oracle.
// It generates no successful response itself: every response comes from that
// oracle, through actual runtime HTTP, with independent outbound shape checks.
import http from 'node:http';
import readline from 'node:readline';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { once } from 'node:events';
const child = spawn(process.execPath, [fileURLToPath(new URL('./support/runtime_server.mjs', import.meta.url))], { stdio: ['pipe', 'pipe', 'inherit'] });
const lines = readline.createInterface({ input: child.stdout });
const replies = [];
let startup;
const ready = new Promise(resolve => { startup = resolve; });
lines.on('line', line => {
  const reply = JSON.parse(line);
  if (startup) { startup(reply); startup = undefined; }
  else replies.shift()?.(reply);
});
const upstream = (await ready).port;
function command(value) {
  return new Promise(resolve => { replies.push(resolve); child.stdin.write(JSON.stringify(value) + '\n'); });
}
const observations = new Map();
const sockets = new Set();
const server = http.createServer(async (req, res) => {
  try {
    const chunks = [];
    let count = 0;
    for await (const chunk of req) {
      count += chunk.length;
      if (count > 1 << 20) throw new Error('request bound');
      chunks.push(chunk);
    }
    const bytes = Buffer.concat(chunks);
    const body = JSON.parse(bytes.toString('utf8'));
    const observation = observations.get(body.model);
    if (observation) {
      observation.stream_seen = body.stream;
      observation.temperature_present = Object.hasOwn(body, 'temperature');
      const calls = body.messages.flatMap(m => m.tool_calls ?? []);
      const results = body.messages.filter(m => m.role === 'tool');
      observation.tool_roundtrip_valid = calls.length === 1 && results.length === 1
        && calls[0].type === 'function' && calls[0].id === results[0].tool_call_id
        && calls[0].function.name === 'synthetic' && JSON.parse(calls[0].function.arguments).value === 'x'.repeat(2048)
        && results[0].content === 'inert synthetic result';
    }
    const forward = http.request({ host: '127.0.0.1', port: upstream, path: req.url, method: req.method,
      headers: { ...req.headers, host: `127.0.0.1:${upstream}` } }, incoming => {
      res.writeHead(incoming.statusCode, incoming.headers);
      res.flushHeaders();
      incoming.pipe(res);
      incoming.on('error', () => res.destroy());
    });
    forward.on('error', () => res.destroy());
    res.on('close', () => forward.destroy());
    forward.end(bytes);
  } catch { res.destroy(); }
});
server.on('connection', socket => { sockets.add(socket); socket.on('close', () => sockets.delete(socket)); });
server.listen(0, '127.0.0.1');
await once(server, 'listening');
process.stdout.write(JSON.stringify({ port: server.address().port }) + '\n');
const control = readline.createInterface({ input: process.stdin });
for await (const line of control) {
  const request = JSON.parse(line);
  if (request.arm) observations.set(request.arm, { stream_seen: null, tool_roundtrip_valid: false, temperature_present: null });
  const result = await command(request);
  process.stdout.write(JSON.stringify({ ...result, ...(observations.get(request.model) ?? {}) }) + '\n');
}
server.close();
for (const socket of sockets) socket.destroy();
child.stdin.end();
await once(child, 'close');
