# Aegisub MCP server

Aegisub can expose the open subtitle projects to AI clients through a local
[Model Context Protocol](https://modelcontextprotocol.io/) Streamable HTTP
endpoint. The server is disabled by default and listens on IPv4 loopback only.

## Enable and connect

1. Open **Preferences > Advanced > MCP Server**.
2. Enable the server. The default port is `27851`; if it is occupied, Aegisub
   selects an available port automatically.
3. Read the connection information from the per-user discovery file:

   - macOS: `~/Library/Application Support/Aegisub/mcp_server.json`
   - other platforms: `mcp_server.json` in Aegisub's user data directory

The discovery file is recreated with a new bearer token whenever the server
starts and is deleted when it stops. On POSIX systems it is readable and
writable only by the current user. Do not copy its token into logs or commit it.

For MCP clients which launch local stdio servers, use the bundled bridge. The
bridge rereads the discovery file for every request, so it keeps working after
Aegisub restarts and rotates the bearer token:

```json
{
  "mcpServers": {
    "aegisub": {
      "command": "/Applications/Aegisub.app/Contents/MacOS/aegisub-mcp-stdio",
      "args": ["stdio"],
      "env": {
        "HOME": "/Users/you"
      }
    }
  }
}
```

If your client runs with a different home directory, pass the discovery file
explicitly:

```json
{
  "mcpServers": {
    "aegisub": {
      "command": "/Applications/Aegisub.app/Contents/MacOS/aegisub-mcp-stdio",
      "args": [
        "stdio",
        "--discovery",
        "/Users/you/Library/Application Support/Aegisub/mcp_server.json"
      ]
    }
  }
}
```

For HTTP-capable clients, the current discovery values can also be registered
directly with:

```sh
D="$HOME/Library/Application Support/Aegisub/mcp_server.json"
TOKEN=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["token"])' "$D")
URL=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["url"])' "$D")
claude mcp add --transport http aegisub "$URL" --header "Authorization: Bearer $TOKEN"
```

Because the token changes after disabling/re-enabling the server, restarting
Aegisub, or changing the port, update the client registration after any of
those actions. Stdio bridge users do not need to update their client
registration because the bridge reads the latest token at call time.

## Capabilities

Clients can list all open Aegisub windows and use their session-unique
`window_id` to:

- inspect project metadata, styles, subtitle lines, and the grid selection;
- find or replace text and metadata, and shift line timing in one undo step;
- create, patch, rename, and safely delete script styles;
- fetch rendered video frames as PNG and line-aligned or explicit audio ranges
  as WAV;
- update subtitle text/original fields in a single undoable batch;
- focus windows, change the selection, or open a subtitle file; and
- read and update existing glossary dictionaries.

Start a workflow with `list_windows`; ids from a closed window are never reused.
Audio clips are limited to 60 seconds. Enable **Read only** to reject mutating
tools while retaining all inspection and media tools.

`get_video_frame` downscales frames to **Frame max width** from **Preferences >
Advanced > MCP Server** (1280 px by default) before returning them. This reduces
image-token use while keeping text readable. A request can override the setting
with `max_width`; use `0` when the original frame size is required. The returned
metadata includes both source and output dimensions.

## Transport and security

The endpoint is `POST /mcp` and uses MCP protocol version `2025-06-18` with
JSON responses (no SSE stream and no persistent MCP session). Requests need the
discovery file's `Authorization: Bearer ...` value. Aegisub rejects non-local
`Host` and `Origin` headers to protect the loopback endpoint from DNS rebinding.
