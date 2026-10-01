// Model-free M2 wire oracle. stdin selects one fixture; stdout is a JSON-line control channel.
// HTTP requests are matched independently of the C++ encoder/decoder, never recorded on failure.
import http from 'node:http';
import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import readline from 'node:readline';
import { isDeepStrictEqual } from 'node:util';
import { once } from 'node:events';

const root = fs.realpathSync(process.argv[2]);
const descriptorDigest = `sha256:${crypto.createHash('sha256').update(fs.readFileSync(process.argv[3])).digest('hex')}`;
const fixtures = new Map();
for (const name of fs.readdirSync(root).filter(x => x.endsWith('.json')).sort()) {
  const fixture = JSON.parse(fs.readFileSync(path.join(root, name), 'utf8'));
  if (fixture.fixture_version !== 2 || fixtures.has(fixture.case_id)) throw new Error('invalid fixture identity');
  fixtures.set(fixture.case_id, fixture);
  const expectedDigest = fixture.descriptor_source === undefined ? descriptorDigest
    : `sha256:${crypto.createHash('sha256').update(fixture.descriptor_source).digest('hex')}`;
  if (fixture.descriptor_digest !== expectedDigest) throw new Error('descriptor digest mismatch');
}
if (fixtures.size === 0) throw new Error('empty fixture corpus');
const reply = value => process.stdout.write(`${JSON.stringify(value)}\n`);
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
let selected;
let stats;
let active = false;
let unexpectedRequests = 0;
const sockets = new Set();

function bytesFor(entry) {
  if (Object.hasOwn(entry, 'utf8') && !Object.hasOwn(entry, 'sidecar')) return Buffer.from(entry.utf8, 'utf8');
  if (Object.hasOwn(entry, 'sidecar') && !Object.hasOwn(entry, 'utf8')) {
    const file = fs.realpathSync(path.resolve(root, entry.sidecar));
    if (!file.startsWith(`${root}${path.sep}`)) throw new Error('sidecar outside fixture directory');
    const data = fs.readFileSync(file);
    if (crypto.createHash('sha256').update(data).digest('hex') !== entry.sha256) throw new Error('sidecar digest mismatch');
    return data;
  }
  throw new Error('invalid byte entry');
}

const server = http.createServer(async (req, res) => {
  if (!selected || active || stats.request_count >= selected.expect.request_count) {
    ++unexpectedRequests;
    if (stats) ++stats.request_count;
    req.resume();
    res.writeHead(409, { 'Content-Length': '0', Connection: 'close' });
    res.end();
    return;
  }
  active = true;
  const fixture = selected;
  const current = stats;
  ++current.request_count;
  try {
    let length = 0;
    const chunks = [];
    for await (const data of req) {
      length += data.length;
      if (length > (1 << 20)) throw new Error('request exceeds bound');
      chunks.push(data);
    }
    const expected = fixture.request;
    if (req.method !== expected.method) current.mismatches.push('method');
    if (req.url !== expected.path) current.mismatches.push('path');
    if (req.headers.host !== `127.0.0.1:${server.address().port}`) current.mismatches.push('authority');
    for (const [name, value] of Object.entries(expected.semantic_headers)) {
      if (req.headers[name.toLowerCase()] !== value) current.mismatches.push(`header:${name.toLowerCase()}`);
    }
    try {
      const actual = JSON.parse(Buffer.concat(chunks).toString('utf8'));
      if (!isDeepStrictEqual(actual, expected.body)) current.mismatches.push('body');
    } catch {
      current.mismatches.push('body-json');
    }
    if (current.mismatches.length) {
      current.consumed = false;
      res.writeHead(400, { 'Content-Length': '0', Connection: 'close' });
      res.end();
      return;
    }
    current.consumed = true;
    const schedule = fixture.transport.schedule;
    const ending = schedule.at(-1);
    if (!ending || !['close', 'reset'].includes(ending.kind)) throw new Error('missing scripted close');
    const bodyLength = schedule.filter(x => x.kind === 'bytes').reduce((sum, x) => sum + bytesFor(x).length, 0);
    const how = ending.kind === 'reset' ? 'rst' : ending.how;
    const headers = { ...fixture.transport.headers, Connection: 'close' };
    if (how === 'content_length_met') headers['Content-Length'] = String(bodyLength);
    else if (how === 'short_body') headers['Content-Length'] = String(bodyLength + 17);
    else if (how === 'chunked_terminator' || how === 'rst') headers['Transfer-Encoding'] = 'chunked';
    else if (how === 'eof_unmarked') res.useChunkedEncodingByDefault = false;
    else throw new Error('unsupported scripted close');
    res.writeHead(fixture.transport.status, headers);
    res.flushHeaders();
    for (const entry of schedule) {
      if (entry.kind === 'bytes') {
        const bytes = bytesFor(entry);
        current.response_bytes += bytes.length;
        if (!res.write(bytes)) await Promise.race([once(res, 'drain'), once(res, 'close')]);
      } else if (entry.kind === 'delay_ms') {
        if (!Number.isSafeInteger(entry.value) || entry.value < 0 || entry.value > 1000) throw new Error('invalid delay');
        await sleep(entry.value);
      } else if (entry.kind === 'close' || entry.kind === 'reset') {
        current.close_fired = how;
        current.fault_fired = ['short_body', 'eof_unmarked', 'rst'].includes(how);
        if (how === 'rst') res.socket.resetAndDestroy();
        else if (how === 'short_body' || how === 'eof_unmarked') res.socket.end();
        else res.end();
      } else throw new Error('unsupported schedule entry');
    }
  } catch (error) {
    current.oracle_error = error.message;
    res.destroy();
  } finally {
    active = false;
  }
});
server.on('connection', socket => {
  sockets.add(socket);
  socket.on('close', () => sockets.delete(socket));
});
server.on('clientError', (_error, socket) => {
  ++unexpectedRequests;
  socket.destroy();
});
server.listen(0, '127.0.0.1', () => reply({ port: server.address().port, fixtures: fixtures.size, descriptor_digest: descriptorDigest }));

const control = readline.createInterface({ input: process.stdin });
control.on('line', line => {
  try {
    const command = JSON.parse(line);
    if (command.arm) {
      if (active) throw new Error('previous fixture still active');
      if (selected && (!stats.consumed || stats.request_count !== selected.expect.request_count) && !command.negative_control) {
        throw new Error('previous fixture unconsumed or count mismatch');
      }
      selected = fixtures.get(command.arm);
      if (!selected) throw new Error('unknown fixture');
      stats = { request_count: 0, mismatches: [], consumed: false, response_bytes: 0, fault_fired: false, close_fired: '', oracle_error: '' };
      reply({ armed: selected.case_id });
    } else if (command.stats) {
      reply({ ...stats, unexpected_requests: unexpectedRequests, active });
    } else throw new Error('unknown control command');
  } catch (error) {
    reply({ error: error.message });
  }
});
control.on('close', () => {
  server.close();
  for (const socket of sockets) socket.destroy();
});
