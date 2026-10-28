#include "../src/main/mcp_framing.h"
#include "check.h"
#include <string>

int main() {
    rs64::mcp::LineBuffer b;
    std::string line;

    b.append("a\nb", 3);
    CHECK(b.pop(line) && line == "a");
    CHECK(!b.pop(line));
    b.append("c\n", 2);
    CHECK(b.pop(line) && line == "bc");

    b.append("crlf\r\n", 6);
    CHECK(b.pop(line) && line == "crlf");

    b.append("\n\n\nx\n", 5);
    CHECK(b.pop(line) && line == "x");
    CHECK(!b.pop(line));

    const std::string big((1u << 20) + 1, 'z');

    b.append(big.data(), big.size());
    CHECK(b.overflowed());
    CHECK(!b.pop(line));
    b.append("tail\nok\n", 8);
    CHECK(b.pop(line) && line == "ok");
    CHECK(!b.overflowed());

    std::printf("mcp_framing_test OK\n");

    return 0;
}
