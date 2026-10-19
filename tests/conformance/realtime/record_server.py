"""A recording WebSocket server for the realtime conformance scenario.

It answers each client message with the scripted server messages for the provider, and writes what the
client sent and what it answered to <out>-client.jsonl and <out>-server.jsonl. Credentials in the
handshake are redacted. Usage: record_server.py <google|openai> <port> <out>
"""
import asyncio
import base64
import json
import sys

import websockets

PROVIDER, PORT, OUT = sys.argv[1], int(sys.argv[2]), sys.argv[3]
PCM24 = base64.b64encode(bytes(range(256)) * 4).decode()  # 1024 bytes of 24 kHz PCM


def google_answers(message):
    """The server messages for one client message of the Google Live scenario. The official SDK sends some
    members in snake_case (`realtime_input`, `client_content`), which the service accepts as their camelCase."""
    message = {"".join(w if i == 0 else w.capitalize() for i, w in enumerate(k.split("_"))): v for k, v in message.items()}
    if "realtimeInput" in message:
        message["realtimeInput"] = {"".join(w if i == 0 else w.capitalize() for i, w in enumerate(k.split("_"))): v for k, v in message["realtimeInput"].items()}
    if "setup" in message:
        return [{"setupComplete": {}}]
    if "clientContent" in message:
        return [{"toolCall": {"functionCalls": [{"id": "call-1", "name": "get_weather", "args": {"city": "Seoul"}}]}}]
    if "toolResponse" in message:
        return [
            {"serverContent": {"modelTurn": {"parts": [{"inlineData": {"mimeType": "audio/pcm;rate=24000", "data": PCM24}}]}}},
            {"serverContent": {"outputTranscription": {"text": "It is 21 degrees in Seoul."}}},
            {"serverContent": {"generationComplete": True}},
            {"serverContent": {"turnComplete": True},
             "usageMetadata": {"promptTokenCount": 120, "responseTokenCount": 40, "totalTokenCount": 160,
                               "promptTokensDetails": [{"modality": "TEXT", "tokenCount": 100}, {"modality": "AUDIO", "tokenCount": 20}],
                               "responseTokensDetails": [{"modality": "AUDIO", "tokenCount": 40}]}},
            {"sessionResumptionUpdate": {"newHandle": "resume-1", "resumable": True}},
            {"goAway": {"timeLeft": "10s"}},
        ]
    if "realtimeInput" in message and message["realtimeInput"].get("audioStreamEnd"):
        return [
            {"serverContent": {"inputTranscription": {"text": "What is the weather?"}}},
            {"serverContent": {"interrupted": True}},
        ]
    return []


def openai_answers(message):
    """The server messages for one client event of the OpenAI Realtime scenario."""
    kind = message.get("type")
    if kind == "session.update":
        return [{"type": "session.updated", "event_id": "ev-2", "session": message.get("session", {})}]
    if kind == "input_audio_buffer.commit":
        return [{"type": "input_audio_buffer.committed", "event_id": "ev-3", "item_id": "item-1"}]
    if kind == "response.create":
        return [
            {"type": "response.created", "event_id": "ev-4", "response": {"id": "resp-1", "object": "realtime.response", "status": "in_progress"}},
            {"type": "response.output_audio.delta", "event_id": "ev-5", "response_id": "resp-1", "item_id": "item-2", "output_index": 0, "content_index": 0, "delta": PCM24},
            {"type": "response.output_audio_transcript.delta", "event_id": "ev-6", "response_id": "resp-1", "item_id": "item-2", "output_index": 0, "content_index": 0, "delta": "It is 21 degrees."},
            {"type": "response.output_audio_transcript.done", "event_id": "ev-7", "response_id": "resp-1", "item_id": "item-2", "output_index": 0, "content_index": 0, "transcript": "It is 21 degrees."},
            {"type": "response.done", "event_id": "ev-8", "response": {"id": "resp-1", "object": "realtime.response", "status": "completed",
             "output": [{"id": "item-2", "type": "message", "role": "assistant", "content": [{"type": "output_audio", "transcript": "It is 21 degrees."}]}],
             "usage": {"total_tokens": 160, "input_tokens": 120, "output_tokens": 40,
                       "input_token_details": {"text_tokens": 100, "audio_tokens": 20, "cached_tokens": 0},
                       "output_token_details": {"text_tokens": 0, "audio_tokens": 40}}}},
        ]
    if kind == "response.cancel":
        return [{"type": "response.done", "event_id": "ev-9", "response": {"id": "resp-2", "object": "realtime.response", "status": "cancelled", "output": [], "usage": None}}]
    return []


async def handler(connection):
    request = connection.request
    redacted = {"authorization", "x-goog-api-key", "api-key"}
    handshake = {"path": request.path, "headers": {k.lower(): ("<redacted>" if k.lower() in redacted else v) for k, v in request.headers.raw_items()}}
    client, server = [], []
    answers = google_answers if PROVIDER == "google" else openai_answers
    if PROVIDER == "openai":
        first = {"type": "session.created", "event_id": "ev-1", "session": {"id": "sess-1", "object": "realtime.session", "model": "gpt-realtime-2.1-mini"}}
        await connection.send(json.dumps(first))
        server.append(first)
    try:
        async for raw in connection:
            message = json.loads(raw)
            client.append(message)
            for answer in answers(message):
                await connection.send(json.dumps(answer))
                server.append(answer)
    except websockets.ConnectionClosed:
        pass
    with open(OUT + "-handshake.json", "w") as f:
        json.dump(handshake, f, indent=1, sort_keys=True)
    with open(OUT + "-client.jsonl", "w") as f:
        f.writelines(json.dumps(m, sort_keys=True) + "\n" for m in client)
    with open(OUT + "-server.jsonl", "w") as f:
        f.writelines(json.dumps(m, sort_keys=True) + "\n" for m in server)


async def main():
    import ssl
    context = None
    if len(sys.argv) > 5:
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(sys.argv[5], sys.argv[6])
    async with websockets.serve(handler, "127.0.0.1", PORT, max_size=16 * 1024 * 1024, ssl=context):
        print("ready", flush=True)
        await asyncio.sleep(float(sys.argv[4]) if len(sys.argv) > 4 else 20)


asyncio.run(main())
