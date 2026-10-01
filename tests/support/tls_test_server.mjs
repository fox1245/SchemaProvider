// Model-free TLS/ALPN oracle. All certificates and keys are ephemeral and loopback-only.
import http from 'node:http';
import http2 from 'node:http2';
import { mkdtempSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';
import { spawnSync } from 'node:child_process';

const names = ['good', 'wrong', 'expired', 'untrusted'];
const counters = Object.fromEntries(names.map(name => [name, {
  requests: 0, tls_errors: 0, h2: 0, h1: 0, request_bytes: 0,
}]));
const servers = [];
const sockets = new Set();
const sessions = new Set();
const startupDeadline = Date.now() + 19_000;
let directory;
let stopping = false;

function cleanup() {
  if (directory !== undefined) {
    rmSync(directory, { recursive: true, force: true });
    directory = undefined;
  }
}

function shutdown(code) {
  if (stopping) return;
  stopping = true;
  for (const session of sessions) session.destroy();
  for (const socket of sockets) socket.destroy();
  for (const server of servers) server.close();
  // exit's synchronous cleanup also runs on startup failure and with held responses.
  process.exit(code);
}

function fail(message) {
  process.stderr.write(`TLS fixture: ${message}\n`);
  shutdown(1);
}

process.on('exit', cleanup);
process.on('SIGINT', () => shutdown(0));
process.on('SIGTERM', () => shutdown(0));
process.stdin.on('end', () => shutdown(0));
process.stdin.on('error', () => fail('stdin failed'));
process.stdin.resume();
const startupTimer = setTimeout(() => fail('startup deadline exceeded'), 19_000);
startupTimer.unref();

function openssl(...args) {
  const remaining = startupDeadline - Date.now();
  if (remaining <= 0) throw new Error('certificate generation deadline exceeded');
  const result = spawnSync('openssl', args, {
    cwd: directory,
    stdio: ['ignore', 'pipe', 'pipe'],
    timeout: Math.min(10_000, remaining),
    killSignal: 'SIGKILL',
    maxBuffer: 1024 * 1024,
  });
  if (result.error || result.status !== 0) {
    // Do not emit subprocess output, certificate material, or request data.
    throw new Error(`openssl ${args[0]} failed (${result.error?.code ?? result.status ?? result.signal})`);
  }
}

function trackSocket(socket) {
  sockets.add(socket);
  socket.on('error', () => {});
  socket.once('close', () => sockets.delete(socket));
}

function ownServer(server) {
  servers.push(server);
  server.on('connection', trackSocket);
  server.on('error', () => fail('listener failed'));
  server.on('clientError', (_error, socket) => {
    socket.on('error', () => {});
    socket.destroy();
  });
  return server;
}

function listen(server) {
  return new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
}

function requestHandler(name, req, res) {
  const count = counters[name];
  const protocol = req.httpVersionMajor === 2 ? 'h2' : 'http/1.1';
  count.requests += 1;
  count[protocol === 'h2' ? 'h2' : 'h1'] += 1;
  const chunks = [];
  let bytes = 0;
  req.on('error', () => res.destroy());
  res.on('error', () => req.destroy());
  req.on('data', chunk => {
    bytes += chunk.length;
    count.request_bytes += chunk.length;
    if (req.url === '/echo') chunks.push(chunk);
  });
  req.on('end', () => {
    if (req.url === '/hold' || res.destroyed) return;
    if (req.url !== '/echo') {
      res.writeHead(404, { 'content-type': 'text/plain' });
      res.end('not found\n');
      return;
    }
    const body = Buffer.concat(chunks, bytes);
    res.writeHead(200, {
      'content-type': 'application/octet-stream',
      'content-length': bytes,
      'x-test-protocol': protocol,
    });
    res.end(body);
  });
}

async function start() {
  // A fixed space-free parent makes the machine-readable ca=<path> token unambiguous.
  directory = mkdtempSync('/tmp/schemaprovider-tls-');
  openssl('req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-sha256',
    '-keyout', 'ca.key', '-out', 'ca.pem', '-days', '2',
    '-subj', '/CN=SchemaProvider ephemeral test CA',
    '-addext', 'basicConstraints=critical,CA:TRUE',
    '-addext', 'keyUsage=critical,keyCertSign,cRLSign');

  const san = 'DNS:localhost,IP:127.0.0.1';
  const extensions = value => `basicConstraints=critical,CA:FALSE\nkeyUsage=critical,digitalSignature,keyEncipherment\nextendedKeyUsage=serverAuth\nsubjectAltName=${value}\n`;
  writeFileSync(join(directory, 'valid.ext'), extensions(san), { mode: 0o600 });
  writeFileSync(join(directory, 'wrong.ext'), extensions('DNS:wrong.invalid'), { mode: 0o600 });
  for (const name of ['good', 'wrong', 'expired']) {
    openssl('req', '-new', '-newkey', 'rsa:2048', '-nodes', '-sha256',
      '-keyout', `${name}.key`, '-out', `${name}.csr`,
      '-subj', `/CN=${name === 'wrong' ? 'wrong.invalid' : 'localhost'}`);
  }
  for (const [index, name] of ['good', 'wrong'].entries()) {
    openssl('x509', '-req', '-in', `${name}.csr`, '-CA', 'ca.pem', '-CAkey', 'ca.key',
      '-set_serial', String(index + 1), '-out', `${name}.pem`, '-days', '2', '-sha256',
      '-extfile', name === 'wrong' ? 'wrong.ext' : 'valid.ext');
  }

  writeFileSync(join(directory, 'index.txt'), '', { mode: 0o600 });
  writeFileSync(join(directory, 'serial'), '03\n', { mode: 0o600 });
  writeFileSync(join(directory, 'ca.cnf'), `
[ca]
default_ca = test_ca
[test_ca]
database = ${directory}/index.txt
serial = ${directory}/serial
new_certs_dir = ${directory}
certificate = ${directory}/ca.pem
private_key = ${directory}/ca.key
default_md = sha256
default_days = 2
policy = subject_policy
unique_subject = no
[subject_policy]
commonName = supplied
`, { mode: 0o600 });
  openssl('ca', '-batch', '-notext', '-config', 'ca.cnf', '-in', 'expired.csr',
    '-out', 'expired.pem', '-extfile', 'valid.ext',
    '-startdate', '20000101000000Z', '-enddate', '20000102000000Z');
  openssl('req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-sha256',
    '-keyout', 'untrusted.key', '-out', 'untrusted.pem', '-days', '2',
    '-subj', '/CN=localhost',
    '-addext', 'basicConstraints=critical,CA:FALSE',
    '-addext', 'keyUsage=critical,digitalSignature,keyEncipherment',
    '-addext', 'extendedKeyUsage=serverAuth', '-addext', `subjectAltName=${san}`);

  const peers = Object.fromEntries(names.map(name => {
    const server = ownServer(http2.createSecureServer({
      key: readFileSync(join(directory, `${name}.key`)),
      cert: readFileSync(join(directory, `${name}.pem`)),
      allowHTTP1: true,
      handshakeTimeout: 5000,
    }, (req, res) => requestHandler(name, req, res)));
    server.on('secureConnection', trackSocket);
    server.on('tlsClientError', (_error, socket) => {
      counters[name].tls_errors += 1;
      socket.on('error', () => {});
      socket.destroy();
    });
    server.on('session', session => {
      sessions.add(session);
      session.on('error', () => {});
      session.once('close', () => sessions.delete(session));
    });
    server.on('stream', stream => stream.on('error', () => {}));
    return [name, server];
  }));
  const stats = ownServer(http.createServer((req, res) => {
    req.on('error', () => res.destroy());
    res.on('error', () => req.destroy());
    req.resume();
    const body = names.flatMap(name => Object.entries(counters[name])
      .map(([key, value]) => `${name}_${key} ${value}\n`)).join('');
    res.writeHead(200, {
      'content-type': 'text/plain',
      'content-length': Buffer.byteLength(body),
      connection: 'close',
    });
    res.end(body);
  }));
  await Promise.all(servers.map(listen));
  clearTimeout(startupTimer);
  const ports = names.map(name => `${name}=${peers[name].address().port}`).join(' ');
  process.stdout.write(`PORTS ${ports} stats=${stats.address().port} ca=${join(directory, 'ca.pem')}\n`);
}

process.stdout.on('error', () => shutdown(1));
start().catch(error => fail(error.message));
