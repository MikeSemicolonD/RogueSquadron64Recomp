#ifndef RS64_MCP_BRIDGE_H
#define RS64_MCP_BRIDGE_H

#include <cstdint>

#include "json/json.hpp"

// ROGUESQ_MCP_PORT=<n> (--mcp-port): newline-delimited JSON on 127.0.0.1:<n> for tools/debug/mcp/rs64_mcp.py; one client at a time.
void rs64_mcp_bridge_start(uint16_t port);
bool rs64_mcp_connected();

// A host method; throw std::runtime_error for an error reply. Each request runs on its own thread, so a handler may block.
using HostHandler = nlohmann::json (*)(const nlohmann::json& params);
void rs64_mcp_register(const char* method, HostHandler handler);

#endif
