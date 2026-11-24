# SPDX-License-Identifier: GPL-3.0-or-later
"""Verify the README's sample edit against the installed editor in package CI."""

import json
import sys
from pathlib import Path
from urllib.request import Request, urlopen


def post(method, params=None, session=None):
    headers = {"Content-Type": "application/json"}
    if session is not None:
        headers["Mcp-Session-Id"] = session
        headers["MCP-Protocol-Version"] = "2025-06-18"
    message = {"jsonrpc": "2.0", "method": method}
    if method != "notifications/initialized":
        message["id"] = method
    if params is not None:
        message["params"] = params
    request = Request(
        "http://127.0.0.1:19899/mcp",
        data=json.dumps(message).encode(),
        headers=headers,
    )
    with urlopen(request, timeout=15) as response:
        body = response.read()
        payload = json.loads(body) if body else {}
        if "error" in payload:
            raise RuntimeError(f"{method} failed: {payload['error']}")
        return payload.get("result"), response.headers.get("Mcp-Session-Id")


def check_sample_edit(path: Path) -> None:
    _, session = post(
        "initialize",
        {
            "protocolVersion": "2025-06-18",
            "capabilities": {},
            "clientInfo": {"name": "sample-check", "version": "1"},
        },
    )
    if session is None:
        raise RuntimeError("MCP initialize returned no session ID")
    post("notifications/initialized", session=session)

    def tool(tool_name, **arguments):
        result, _ = post("tools/call", {"name": tool_name, "arguments": arguments}, session)
        if result["isError"]:
            raise RuntimeError(f"{tool_name} failed: {result['content']}")
        return result["structuredContent"]

    second = 1_000_000_000
    sample_path = path.resolve(strict=True)
    tool("project.create", name="First Cut", width=960, height=540, fps=30)
    asset = tool("media.import", path=str(sample_path))
    if not asset["hasVideo"] or asset["durationMs"] != 8000:
        raise RuntimeError(f"Expected an eight-second video: {asset}")
    track = tool("timeline.add_track", kind="video")
    clip = tool(
        "timeline.add_clip",
        trackId=track["trackId"],
        assetId=asset["assetId"],
        sourcePath=asset["sourcePath"],
        timelineStartNs=0,
        sourceInNs=2 * second,
        sourceOutNs=6 * second,
    )
    tool(
        "timeline.set_clip_properties",
        clipId=clip["clipId"],
        fadeInNs=second // 2,
        fadeOutNs=second // 2,
    )
    timeline = tool("timeline.read")
    clips = [item for track in timeline["tracks"] for item in track["clips"]]
    if (
        len(clips) != 1
        or clips[0]["id"] != clip["clipId"]
        or clips[0]["timelineStartNs"] != 0
        or clips[0]["sourceInNs"] != 2 * second
        or clips[0]["sourceOutNs"] != 6 * second
        or timeline["durationNs"] != 4 * second
    ):
        raise RuntimeError(f"Expected one four-second clip at timeline zero: {timeline}")
    tool(
        "timeline.capture_frame",
        outputPath=str(sample_path.with_name("palmier-sample-frame.png")),
        positionNs=second,
    )
    print("Sample edit passed: imported, trimmed, applied both fades, and rendered a frame.")


if __name__ == "__main__":
    check_sample_edit(Path(sys.argv[1]))
