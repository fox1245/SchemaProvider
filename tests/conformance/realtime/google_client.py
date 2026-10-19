"""The Google Live side of the conformance scenario, through the official google-genai SDK."""
import asyncio
import sys

from google import genai
from google.genai import types

PORT = int(sys.argv[1])
PCM16 = bytes(range(256)) * 25  # 6400 bytes: 0.2 s of 16 kHz mono PCM


async def main():
    client = genai.Client(api_key="test-key", http_options=types.HttpOptions(base_url=f"http://127.0.0.1:{PORT}", api_version="v1beta"))
    config = types.LiveConnectConfig(
        response_modalities=["AUDIO"],
        system_instruction="Answer briefly.",
        speech_config=types.SpeechConfig(voice_config=types.VoiceConfig(prebuilt_voice_config=types.PrebuiltVoiceConfig(voice_name="Puck"))),
        input_audio_transcription=types.AudioTranscriptionConfig(),
        output_audio_transcription=types.AudioTranscriptionConfig(),
        session_resumption=types.SessionResumptionConfig(),
        tools=[types.Tool(function_declarations=[types.FunctionDeclaration(
            name="get_weather", description="The weather in a city.",
            parameters=types.Schema(type="OBJECT", properties={"city": types.Schema(type="STRING")}, required=["city"]))])],
    )
    async with client.aio.live.connect(model="gemini-3.8-live", config=config) as session:
        await session.send_realtime_input(audio=types.Blob(data=PCM16, mime_type="audio/pcm;rate=16000"))
        await session.send_realtime_input(audio_stream_end=True)
        await session.send_client_content(turns=types.Content(role="user", parts=[types.Part(text="And in Seoul?")]), turn_complete=True)
        await session.send_tool_response(function_responses=[types.FunctionResponse(id="call-1", name="get_weather", response={"temperature": 21})])
        await asyncio.sleep(0.5)


asyncio.run(main())
