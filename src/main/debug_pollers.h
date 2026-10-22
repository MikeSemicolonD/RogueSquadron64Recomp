#pragma once

// Background threads that log game state from RDRAM: ROGUESQ_LOG_STATE (scene state + frame callbacks) and ROGUESQ_LOG_PHASE (slot dispatcher). Each returns at once when its variable is unset.
namespace rs64::diag {
    void start_state_poller();
    void start_phase_poller();
}
