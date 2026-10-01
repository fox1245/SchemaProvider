// Cleartext HTTP/2 (prior knowledge) flood server for the transport backpressure experiment
// (docs/POC_PLAN.md risk R3). Prints "PORTS h2=<p> stats=<p>" once listening and exits when stdin
// reaches EOF. GET http://127.0.0.1:<stats>/ returns "written <bytes>\ndone <n>\n" over HTTP/1.1.
import http from 'node:http';
import http2 from 'node:http2';

let written = 0;
let done = 0;
const block = Buffer.alloc(64 * 1024, 'z');

const h2 = http2.createServer();
h2.on('stream', (stream, headers) => {
  const url = new URL(headers[':path'], 'http://x');
  const total = Number(url.searchParams.get('total') ?? 1048576);
  stream.on('error', () => {});
  stream.respond({ ':status': 200, 'content-length': String(total) });
  let sent = 0;
  const pump = () => {
    while (sent < total) {
      const n = Math.min(block.length, total - sent);
      const ok = stream.write(block.subarray(0, n));
      sent += n;
      written += n;
      if (!ok) {
        stream.once('drain', pump);
        return;
      }
    }
    stream.end();
    done += 1;
  };
  pump();
});

const stats = http.createServer((_req, res) => {
  res.setHeader('connection', 'close');
  res.end(`written ${written}\ndone ${done}\n`);
});

h2.listen(0, '127.0.0.1', () => {
  stats.listen(0, '127.0.0.1', () => {
    console.log(`PORTS h2=${h2.address().port} stats=${stats.address().port}`);
  });
});

process.stdin.on('end', () => process.exit(0));
process.stdin.resume();
