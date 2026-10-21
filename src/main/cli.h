#pragma once

#ifndef RS64_VERSION_STRING
#define RS64_VERSION_STRING "unknown"
#endif

// Command-line flags. Each one sets a ROGUESQ_* environment variable, which is how the rest of the program is configured.
namespace rs64::cli {
    // Returns an exit code to stop startup (0 for --help, 2 for a bad option), or -1 to continue.
    int apply_args(int argc, char* argv[]);
}
