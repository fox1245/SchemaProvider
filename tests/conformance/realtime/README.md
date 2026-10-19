# Realtime conformance recordings

The fixtures in `tests/fixtures/realtime` were recorded by running one scripted scenario through the official SDKs
against `record_server.py`, a WebSocket server that answers with the scenario's server messages and writes what each
side sent. `realtime_properties` then checks that the SDK's codecs send the same frames for the same setup and
commands, once canonicalized, and decode the recorded server frames.

To record again, with a Python that has `google-genai`, `openai` and `websockets`:

```sh
openssl req -x509 -newkey rsa:2048 -nodes -keyout key.pem -out cert.pem -days 1 \
  -subj /CN=127.0.0.1 -addext subjectAltName=IP:127.0.0.1
# google-genai always connects over TLS when it has an API key
python record_server.py google 18801 google 90 cert.pem key.pem &
SSL_CERT_FILE=$PWD/cert.pem python google_client.py 18801
python record_server.py openai 18802 openai 90 &
python openai_client.py 18802
```

Each run writes `<provider>-client.jsonl`, `<provider>-server.jsonl` and `<provider>-handshake.json` (credentials
redacted). The fixtures of 2026-10-20 came from google-genai 2.22.0 and openai 3.3.0.
