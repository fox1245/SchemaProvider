"""Model-free HTTP/3 oracle. Isolated aioquic==1.3.0 + h2==4.3.0 venv only.

stdin: STATS <id>, RELEASE <id>, WAIT <id> <counter> <minimum>, EXIT.
stdout is a bounded line protocol. TCP TLS and UDP QUIC share the dual port;
fallback has TCP TLS plus a silent UDP listener; blackhole has silent UDP only.
Certificates follow tls_test_server.mjs's ephemeral CA + localhost/IP SAN model.
"""
import asyncio
import collections
import datetime
import ipaddress
import pathlib
import signal
import ssl
import sys
import tempfile

from aioquic.asyncio import QuicConnectionProtocol, serve
from aioquic.h3.connection import H3Connection, H3_ALPN
from aioquic.h3.events import HeadersReceived, DataReceived
from aioquic.quic.configuration import QuicConfiguration
from aioquic.quic.events import ProtocolNegotiated, HandshakeCompleted, StreamReset, StopSendingReceived, ConnectionTerminated
from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import rsa
from cryptography.x509.oid import NameOID
from h2.connection import H2Connection
from h2.config import H2Configuration
from h2.events import RequestReceived, DataReceived as H2DataReceived, StreamEnded

BODY = b'{"ok":true}'
SSE = b'data: token\n\ndata: [DONE]\n\n'
TOTAL = 64 << 20
CHUNK = b'x' * 16384


class State:
    def __init__(self):
        self.rows = collections.defaultdict(lambda: collections.defaultdict(int))
        self.releases = {}
        self.changed = asyncio.Event()
        self.next_connection = 0
        self.tasks = set()
        self.fixture_errors = 0

    def bump(self, key, field, amount=1):
        self.rows[key][field] += amount
        self.changed.set()

    def task(self, coroutine):
        task = asyncio.create_task(coroutine)
        self.tasks.add(task)
        def complete(task):
            self.tasks.discard(task)
            if not task.cancelled() and task.exception() is not None:
                self.fixture_errors += 1
                self.changed.set()
        task.add_done_callback(complete)
        return task

    def accept(self, path, protocol, connection, method, body):
        parts = path.split('/')
        if len(parts) != 3 or not parts[2].isalnum():
            raise ValueError('invalid fixture path')
        mode, key = parts[1:]
        self.bump(key, 'requests')
        self.bump(key, protocol)
        self.bump(key, 'request_bytes', len(body))
        self.rows[key]['connection'] = connection
        if method != 'POST' or body != b'{}':
            self.bump(key, 'invalid')
        return mode, key


class PeerProtocol(QuicConnectionProtocol):
    def __init__(self, *args, state, **kwargs):
        super().__init__(*args, **kwargs)
        self.state = state
        state.next_connection += 1
        self.connection = state.next_connection
        self.http = None
        self.requests = {}
        self.closed = False
        self.responses = set()
        self.early_data = False

    def quic_event_received(self, event):
        if isinstance(event, ProtocolNegotiated):
            self.http = H3Connection(self._quic)
        elif isinstance(event, HandshakeCompleted):
            self.early_data = event.early_data_accepted
        elif isinstance(event, ConnectionTerminated):
            self.closed = True
            for task in self.responses:
                task.cancel()
        elif isinstance(event, (StreamReset, StopSendingReceived)):
            entry = self.requests.get(event.stream_id)
            if entry and 'key' in entry:
                self.state.bump(entry['key'], 'client_reset')
        if self.http is None:
            return
        for item in self.http.handle_event(event):
            if isinstance(item, HeadersReceived):
                fields = dict(item.headers)
                entry = self.requests.setdefault(item.stream_id, {'body': bytearray()})
                entry['path'] = fields.get(b':path', b'').decode('ascii')
                entry['method'] = fields.get(b':method', b'').decode('ascii')
                if item.stream_ended:
                    self.dispatch(item.stream_id)
            elif isinstance(item, DataReceived):
                entry = self.requests[item.stream_id]
                if len(entry['body']) + len(item.data) > 4096:
                    self.close(error_code=0x107, reason_phrase='fixture request limit')
                    return
                entry['body'].extend(item.data)
                if item.stream_ended:
                    self.dispatch(item.stream_id)

    def dispatch(self, stream):
        entry = self.requests[stream]
        mode, key = self.state.accept(entry['path'], 'h3', self.connection,
                                      entry['method'], bytes(entry['body']))
        entry['key'] = key
        self.state.bump(key, 'early_data', int(self.early_data))
        task = self.state.task(self.respond(stream, mode, key))
        self.responses.add(task)
        task.add_done_callback(self.responses.discard)

    def headers(self, stream, content_type=b'application/json', length=None):
        fields = [(b':status', b'200'), (b'content-type', content_type), (b'x-test-protocol', b'h3')]
        if length is not None:
            fields.append((b'content-length', str(length).encode('ascii')))
        self.http.send_headers(stream, fields)
        self.transmit()

    async def respond(self, stream, mode, key):
        if mode == 'hold':
            await self.state.releases.setdefault(key, asyncio.Event()).wait()
            self.headers(stream)
            self.http.send_data(stream, BODY, end_stream=True)
        elif mode == 'sse':
            self.headers(stream, b'text/event-stream')
            for part in [b'data: token\n\n', b'data: [DONE]\n\n']:
                self.http.send_data(stream, part, end_stream=part.endswith(b'[DONE]\n\n'))
                self.transmit()
                await asyncio.sleep(0.01)
        elif mode in ('ssemissing', 'ssetruncate', 'ssehold'):
            self.headers(stream, b'text/event-stream')
            self.http.send_data(stream, b'data: token\n\n', end_stream=mode == 'ssemissing')
            self.transmit()
            if mode == 'ssetruncate':
                await asyncio.sleep(0.03)
                self._quic.reset_stream(stream, error_code=0x102)
                self.state.bump(key, 'faults')
            elif mode == 'ssehold':
                await self.state.releases.setdefault(key, asyncio.Event()).wait()
                self.http.send_data(stream, b'data: [DONE]\n\n', end_stream=True)
        elif mode == 'reset':
            self._quic.reset_stream(stream, error_code=0x10b)
            self.state.bump(key, 'faults')
        elif mode == 'truncate':
            self.headers(stream, length=100)
            self.http.send_data(stream, b'cut', end_stream=False)
            self.transmit()
            await asyncio.sleep(0.03)
            self._quic.reset_stream(stream, error_code=0x102)
            self.state.bump(key, 'faults')
        elif mode == 'short':
            self.headers(stream, length=100)
            self.http.send_data(stream, b'cut', end_stream=True)
            self.state.bump(key, 'faults')
        elif mode == 'flood':
            self.headers(stream, b'application/octet-stream', TOTAL)
            while self.state.rows[key]['produced'] < TOTAL and not self.closed:
                quic_stream = self._quic._streams.get(stream)
                if quic_stream is None:
                    break
                sender = quic_stream.sender
                # Pinned aioquic sender buffer contains unacknowledged bytes. Bound
                # fixture memory without hiding QUIC stream flow-control progress.
                if sender.is_finished or sender.reset_pending or sender._reset_error_code is not None:
                    break
                if sender._buffer_stop - sender._buffer_start >= (256 << 10):
                    await asyncio.sleep(0.005)
                    continue
                self.http.send_data(stream, CHUNK, end_stream=False)
                self.state.bump(key, 'produced', len(CHUNK))
                self.transmit()
                await asyncio.sleep(0)
            if not self.closed and self.state.rows[key]['produced'] == TOTAL:
                self.http.send_data(stream, b'', end_stream=True)
        else:
            self.headers(stream)
            self.http.send_data(stream, BODY, end_stream=True)
        self.transmit()
        self.state.bump(key, 'ended')


class SilentUDP(asyncio.DatagramProtocol):
    pass


def certificates(directory):
    now = datetime.datetime.now(datetime.timezone.utc)
    ca_key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    ca_name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, 'SchemaProvider ephemeral test CA')])
    name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, 'localhost')])
    def base(subject, issuer, public, serial):
        return (x509.CertificateBuilder().subject_name(subject).issuer_name(issuer)
                .public_key(public).serial_number(serial).not_valid_before(now - datetime.timedelta(minutes=5))
                .not_valid_after(now + datetime.timedelta(days=2)))
    ca = (base(ca_name, ca_name, ca_key.public_key(), 1)
          .add_extension(x509.BasicConstraints(ca=True, path_length=None), critical=True)
          .sign(ca_key, hashes.SHA256()))
    cert = (base(name, ca_name, key.public_key(), 2)
            .add_extension(x509.BasicConstraints(ca=False, path_length=None), critical=True)
            .add_extension(x509.SubjectAlternativeName([x509.DNSName('localhost'),
                           x509.IPAddress(ipaddress.ip_address('127.0.0.1'))]), critical=False)
            .sign(ca_key, hashes.SHA256()))
    paths = [pathlib.Path(directory) / n for n in ['ca.pem', 'good.pem', 'good.key']]
    paths[0].write_bytes(ca.public_bytes(serialization.Encoding.PEM))
    paths[1].write_bytes(cert.public_bytes(serialization.Encoding.PEM))
    paths[2].write_bytes(key.private_bytes(serialization.Encoding.PEM,
                         serialization.PrivateFormat.PKCS8, serialization.NoEncryption()))
    paths[2].chmod(0o600)
    return paths


async def main(directory):
    state = State()
    loop = asyncio.get_running_loop()
    main_task = asyncio.current_task()
    stopping = False
    def request_stop():
        nonlocal stopping
        if not stopping:
            stopping = True
            main_task.cancel()
    loop.add_signal_handler(signal.SIGTERM, request_stop)
    ca, cert, key = certificates(directory)
    tls = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    tls.load_cert_chain(cert, key)
    tls.set_alpn_protocols(['h2', 'http/1.1'])
    writers = set()

    async def tcp_h2(reader, writer, connection):
        http = H2Connection(config=H2Configuration(client_side=False, header_encoding='ascii'))
        http.initiate_connection()
        writer.write(http.data_to_send())
        await writer.drain()
        streams = {}
        while data := await reader.read(65536):
            for event in http.receive_data(data):
                if isinstance(event, RequestReceived):
                    streams[event.stream_id] = {'headers': dict(event.headers), 'body': bytearray()}
                elif isinstance(event, H2DataReceived):
                    entry = streams[event.stream_id]
                    if len(entry['body']) + len(event.data) > 4096:
                        raise ValueError('fixture body bound')
                    entry['body'].extend(event.data)
                    http.acknowledge_received_data(event.flow_controlled_length, event.stream_id)
                elif isinstance(event, StreamEnded):
                    entry = streams.pop(event.stream_id)
                    headers = entry['headers']
                    mode, ident = state.accept(headers[':path'], 'tcp', connection,
                                               headers[':method'], bytes(entry['body']))
                    state.bump(ident, 'h2')
                    if mode == 'hold':
                        await state.releases.setdefault(ident, asyncio.Event()).wait()
                    payload = SSE if mode == 'sse' else BODY
                    http.send_headers(event.stream_id, [(':status', '200'),
                                      ('content-length', str(len(payload))), ('x-test-protocol', 'h2')])
                    http.send_data(event.stream_id, payload, end_stream=True)
                    state.bump(ident, 'ended')
            writer.write(http.data_to_send())
            await writer.drain()

    async def tcp(reader, writer):
        writers.add(writer)
        state.next_connection += 1
        connection = state.next_connection
        try:
            if writer.get_extra_info('ssl_object').selected_alpn_protocol() == 'h2':
                await tcp_h2(reader, writer, connection)
                return
            while True:
                head = await reader.readuntil(b'\r\n\r\n')
                lines = head.decode('ascii').split('\r\n')
                method, path, _ = lines[0].split(' ')
                fields = dict(line.lower().split(':', 1) for line in lines[1:] if ':' in line)
                size = int(fields.get('content-length', '0'))
                if not 0 <= size <= 4096:
                    raise ValueError('fixture body bound')
                body = await reader.readexactly(size)
                mode, ident = state.accept(path, 'tcp', connection, method, body)
                state.bump(ident, 'h1')
                if mode == 'hold':
                    await state.releases.setdefault(ident, asyncio.Event()).wait()
                payload = SSE if mode == 'sse' else BODY
                writer.write(b'HTTP/1.1 200 OK\r\nContent-Length: ' + str(len(payload)).encode()
                             + b'\r\nX-Test-Protocol: http/1.1\r\n\r\n' + payload)
                await writer.drain()
                state.bump(ident, 'ended')
        except (asyncio.IncompleteReadError, ConnectionError, ssl.SSLError):
            pass
        finally:
            writers.discard(writer)
            writer.close()

    servers = []
    transports = []
    quic = None
    try:
        dual = await asyncio.start_server(tcp, '127.0.0.1', 0, ssl=tls)
        servers.append(dual)
        port = dual.sockets[0].getsockname()[1]
        config = QuicConfiguration(is_client=False, alpn_protocols=H3_ALPN,
                                   max_data=128 << 20, max_stream_data=1 << 20)
        config.load_cert_chain(cert, key)
        # No session-ticket handler/fetcher: no resumption or 0-RTT acceptance.
        quic = await serve('127.0.0.1', port, configuration=config,
                           create_protocol=lambda *a, **kw: PeerProtocol(*a, state=state, **kw))
        fallback = await asyncio.start_server(tcp, '127.0.0.1', 0, ssl=tls)
        servers.append(fallback)
        fallback_port = fallback.sockets[0].getsockname()[1]
        for p in [fallback_port, 0]:
            transport, _ = await loop.create_datagram_endpoint(SilentUDP, local_addr=('127.0.0.1', p))
            transports.append(transport)
        blackhole = transports[-1].get_extra_info('sockname')[1]
        print(f'READY dual={port} fallback={fallback_port} blackhole={blackhole} ca={ca}', flush=True)
        reader = asyncio.StreamReader(limit=4096)
        await loop.connect_read_pipe(lambda: asyncio.StreamReaderProtocol(reader), sys.stdin)
        while line := await reader.readline():
            parts = line.decode('ascii').strip().split()
            if parts == ['EXIT']:
                break
            if len(parts) < 2 or not parts[1].isalnum():
                raise ValueError('invalid control command')
            command, ident = parts[:2]
            if command == 'RELEASE':
                state.releases.setdefault(ident, asyncio.Event()).set()
            elif command == 'WAIT' and len(parts) == 4:
                async def wait():
                    while state.rows[ident][parts[2]] < int(parts[3]):
                        state.changed.clear()
                        await state.changed.wait()
                await asyncio.wait_for(wait(), 10)
            elif command != 'STATS':
                raise ValueError('invalid control command')
            print('OK fixture_errors=' + str(state.fixture_errors) + ' ' +
                  ' '.join(f'{k}={v}' for k, v in sorted(state.rows[ident].items())), flush=True)
    finally:
        stopping = True
        for task in tuple(state.tasks):
            task.cancel()
        if state.tasks:
            await asyncio.gather(*state.tasks, return_exceptions=True)
        if quic:
            quic.close()
        for transport in transports:
            transport.close()
        for server in servers:
            server.close()
        # Server.wait_closed waits for accepted clients on Python 3.12. Close
        # their owned sockets first, without awaiting a peer TLS close-notify.
        for writer in tuple(writers):
            writer.transport.abort()
        for server in servers:
            await server.wait_closed()




if __name__ == '__main__':
    try:
        with tempfile.TemporaryDirectory(prefix='schemaprovider-h3-') as directory:
            asyncio.run(main(directory))
    except (KeyboardInterrupt, asyncio.CancelledError):
        pass
    except Exception as error:
        print('HTTP3 fixture failed: ' + type(error).__name__, file=sys.stderr)
        sys.exit(1)
