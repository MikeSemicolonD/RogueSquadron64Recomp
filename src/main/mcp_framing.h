#pragma once

#include <cstddef>
#include <string>

namespace rs64::mcp {
// Newline-delimited message framing for the MCP bridge; a line over 1 MiB is dropped whole.
struct LineBuffer
{
    static constexpr size_t kMaxLine = size_t(1) << 20;

    void append(const char* p, size_t n)
    {

        for (size_t i = 0; i < n; i++)
        {
            if (m_skipping) {
                m_skipping = p[i] != '\n';

                continue;
            }

            m_pending.push_back(p[i]);

            if (p[i] == '\n') {
                m_ready += m_pending;
                m_pending.clear();
            }
            else if (m_pending.size() > kMaxLine) {
                m_pending.clear();
                m_overflowed = true;
                m_skipping = true;
            }
        }
    }

    bool pop(std::string& line)
    {

        while (!m_ready.empty())
        {
            const size_t nl = m_ready.find('\n');

            line.assign(m_ready, 0, nl);
            m_ready.erase(0, nl + 1);

            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }

            if (!line.empty()) {
                m_overflowed = false;

                return true;
            }
        }

        return false;
    }

    bool overflowed() const
    {

        return m_overflowed;
    }

private:
    std::string m_pending;
    std::string m_ready;
    bool m_overflowed = false;
    bool m_skipping = false;
};
}
