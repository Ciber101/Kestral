#pragma once
// Kestral's MCP server: other AI apps (Claude Desktop, Claude Code, Cursor, any MCP client) use the
// same tools as Kestral's own assistant, live in the open Kestral window.
//
// Two ways in:
//   * Streamable HTTP at http://127.0.0.1:<port>/mcp (only this computer) with the header
//     "Authorization: Bearer <key>". Port and key are in Preferences > Assistant > MCP server and
//     in <app data>/mcp.json.
//   * "Kestral.exe --mcp": a stdio bridge for apps that start a program. It finds the open Kestral
//     (or starts it) through mcp.json and passes every message along.
#include <string>

namespace kst {

const char* kestralVersion();

namespace mcp {

constexpr int kDefaultPort = 47800;

// The stdio bridge ("--mcp"). Returns the process exit code.
int runBridge(bool readOnly = false);   // readOnly: use the key that can only look

// Path of the file that tells the bridge where the server is (<app data>/mcp.json).
std::string infoPath();

} // namespace mcp
} // namespace kst
