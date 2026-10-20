# Realtime conformance recordings

The fixtures in `tests/fixtures/realtime` come from running one scripted scenario through the official SDKs against
`record_server.py`, a WebSocket server that answers with scripted server messages and writes what each side sent. The
`*-client.jsonl` and `*-handshake.json` files are what the official SDKs sent. The `google-server.jsonl` and
`openai-server.jsonl` files are the script `record_server.py` plays, not frames a provider sent; the transcription
fixtures (`*-transcription-server.jsonl`) were recorded from live sessions. `realtime_properties` checks that the SDK's
codecs send the same frames for the same setup and commands, once canonicalized, and decode the server frames.

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

Each run writes `<provider>-client.jsonl`, `<provider>-server.jsonl` (the script as played) and `<provider>-handshake.json`
(credentials redacted). The fixtures of 2026-10-20 came from google-genai 2.22.0 and openai 3.3.0.
