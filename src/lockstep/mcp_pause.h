#pragma once
// The MCP pause server (mcp_pause.cpp, RS64_MCP builds only) and the hold it shares with lockstep.cpp's pause points.
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>

// Holds the calling game thread until released() or the deadline; true when released.
bool hold_until(const std::function<bool()>& released, std::chrono::steady_clock::time_point until);

// Called from rs64_pause_point on the game thread: records the clock's frame, then holds if an MCP pause target is reached.
void mcp_note_clock(const std::string& clock, uint32_t frame);
void mcp_pause_point(const std::string& clock);
