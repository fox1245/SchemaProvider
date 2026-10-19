"""The OpenAI Realtime side of the conformance scenario, through the official openai SDK."""
import asyncio
import base64
import sys

import openai

PORT = int(sys.argv[1])
PCM24 = base64.b64encode(bytes(range(256)) * 25).decode()  # 6400 bytes of 24 kHz mono PCM


async def main():
    client = openai.AsyncOpenAI(api_key="test-key", websocket_base_url=f"ws://127.0.0.1:{PORT}/v1")
    async with client.realtime.connect(model="gpt-realtime-2.1-mini") as connection:
        await connection.session.update(session={
            "type": "realtime",
            "instructions": "Answer briefly.",
            "output_modalities": ["audio"],
            "audio": {"input": {"format": {"type": "audio/pcm", "rate": 24000}, "transcription": {"model": "gpt-live-transcribe"}},
                      "output": {"format": {"type": "audio/pcm", "rate": 24000}, "voice": "alloy"}},
            "tools": [{"type": "function", "name": "get_weather", "description": "The weather in a city.",
                       "parameters": {"type": "object", "properties": {"city": {"type": "string"}}, "required": ["city"]}}],
        })
        await connection.input_audio_buffer.append(audio=PCM24)
        await connection.input_audio_buffer.commit()
        await connection.conversation.item.create(item={"type": "message", "role": "user", "content": [{"type": "input_text", "text": "And in Seoul?"}]})
        await connection.conversation.item.create(item={"type": "function_call_output", "call_id": "call-1", "output": "{\"temperature\":21}"})
        await connection.response.create()
        await connection.response.cancel()
        await asyncio.sleep(0.5)


asyncio.run(main())
