"""Official `anthropic` Python SDK: the single-provider baseline for the Messages API (AsyncAnthropic, asyncio)."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import harness  # noqa: E402

import anthropic  # noqa: E402


def setup(a):
    if a.api != "anthropic":
        raise SystemExit("anthropic supports the Messages cells only")
    limits, http_lib = harness.sdk_limits(anthropic.DefaultAsyncHttpxClient, a.concurrency)
    a.http_lib = http_lib
    http = anthropic.DefaultAsyncHttpxClient(limits=limits)
    return anthropic.AsyncAnthropic(api_key=a.api_key, base_url=a.base_prefix, http_client=http)


def request(a):
    return dict(model=a.model, max_tokens=a.max_tokens, system=a.system, messages=[{"role": "user", "content": a.user}])


async def call(client, a):
    if a.mode == "buffered":
        return await client.messages.create(**request(a))
    parts = []
    # The README's streaming helper: it accumulates the message as well as yielding text.
    async with client.messages.stream(**request(a)) as stream:
        async for text in stream.text_stream:
            if text:
                parts.append(text)
    return parts


def extract(message):
    return "".join(block.text for block in message.content if block.type == "text")


def describe(a):
    return {
        "concurrency_model": "asyncio, one AsyncAnthropic client shared by all tasks", "http": "1.1 (HTTP/2 not enabled)",
        "http_library": a.http_lib, "pool": f"max_connections=max_keepalive_connections={a.concurrency}",
        "ca": "SSL_CERT_FILE through the HTTP library's default trust", "retries": "SDK default (max_retries=2), not triggered",
        "telemetry": "none in the SDK", "stream": "client.messages.stream(...).text_stream helper",
    }


if __name__ == "__main__":
    harness.main("anthropic-py", anthropic.__version__, setup, call, extract, describe)
