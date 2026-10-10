"""A LangGraph tool-calling agent whose LLM node is SchemaProvider, called through ctypes.

Run against the credential-free peer:   python fake_chat_peer.py 8765 &   python langgraph_agent.py
Run against a real endpoint by passing --descriptor/--model/--dotenv/--key-name. C++ reads the
key into this process's native memory, without returning it to Python or exporting it.
"""
from __future__ import annotations

import argparse
import json
import operator
import time
from concurrent.futures import ThreadPoolExecutor
from typing import Annotated, Any, TypedDict

from langgraph.graph import END, START, StateGraph

from schemaprovider_ctypes import Client, Conversation

ADD_TOOL = {"name": "add", "description": "Add two integers.",
            "parameters": {"type": "object", "properties": {"a": {"type": "integer"}, "b": {"type": "integer"}},
                           "required": ["a", "b"], "additionalProperties": False}}


def add(arguments: dict[str, Any]) -> str:
    if set(arguments) != {"a", "b"} or any(type(arguments[key]) is not int for key in ("a", "b")):
        raise ValueError("add requires exactly two integer arguments a and b")
    return str(arguments["a"] + arguments["b"])


LOCAL_TOOLS = {"add": add}


class State(TypedDict):
    question: str
    tool_calls: list[dict[str, Any]]
    tool_results: list[dict[str, str]]
    answer: str
    trace: Annotated[list[str], operator.add]


def build_graph(conversation: Conversation):
    def llm(state: State) -> dict[str, Any]:
        # First visit sends the question; later visits send the tool results of the last answer.
        outcome = (conversation.tool_results(state["tool_results"]) if state["tool_results"]
                   else conversation.say(state["question"]))
        return {"tool_calls": outcome["tool_calls"], "answer": outcome["text"], "tool_results": [],
                "trace": [f"llm stop={outcome['stop']} usage={outcome['usage']}"]}

    def tools(state: State) -> dict[str, Any]:
        results = [{"call_id": call["id"], "content": LOCAL_TOOLS[call["name"]](call["arguments"])}
                   for call in state["tool_calls"]]
        return {"tool_results": results, "tool_calls": [],
                "trace": ["tools ran " + ",".join(call["name"] for call in state["tool_calls"])]}

    graph = StateGraph(State)
    graph.add_node("llm", llm)
    graph.add_node("tools", tools)
    graph.add_edge(START, "llm")
    graph.add_conditional_edges("llm", lambda state: "tools" if state["tool_calls"] else END)
    graph.add_edge("tools", "llm")
    return graph.compile()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--base-url", default="http://127.0.0.1:8765")
    parser.add_argument("--descriptor", help="path to a SchemaProvider descriptor JSON (family openai.chat)")
    parser.add_argument("--model", default="example-model")
    parser.add_argument("--dotenv")
    parser.add_argument("--key-name")
    parser.add_argument("--question", default="What is 2 + 3? Use the add tool.")
    args = parser.parse_args()

    descriptor = (json.load(open(args.descriptor)) if args.descriptor else {
        "descriptor_version": 1, "revision": 1, "id": "ctypes-example", "family": "openai.chat",
        "connection": {"base_url": args.base_url,
                       "paths": {"buffered": "/v1/chat/completions", "streaming": "/v1/chat/completions"}}})
    with Client(descriptor, dotenv_path=args.dotenv, api_key_name=args.key_name) as client:
        print("interface revision:", client.interface_revision)
        with client.conversation(args.model, tools=[ADD_TOOL]) as conversation:
            final = build_graph(conversation).invoke({"question": args.question, "tool_calls": [], "tool_results": [],
                                                      "answer": "", "trace": []})
        print("\n".join(final["trace"]))
        print("answer:", final["answer"])

        # Independent chats share one client. Their native waits overlap; each tool loop still
        # has two sequential model requests, and Python graph/tool work remains under the GIL.
        def one(index: int) -> str:
            with client.conversation(args.model, tools=[ADD_TOOL]) as chat:
                return build_graph(chat).invoke({"question": f"What is {index} + 10? Use the add tool.", "tool_calls": [],
                                                 "tool_results": [], "answer": "", "trace": []})["answer"]

        started = time.perf_counter()
        with ThreadPoolExecutor(max_workers=4) as pool:
            answers = list(pool.map(one, range(4)))
        print("parallel answers:", answers, "in %.2f s" % (time.perf_counter() - started))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
