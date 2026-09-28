#!/usr/bin/env bash
# Register a one-job (ephemeral) runner, run it, and deregister on shutdown.
# Needs GITHUB_REPOSITORY (owner/repo) and GITHUB_PAT (fine-grained, this repo only, Administration: read/write).
# Runs as root so the PAT stays in root-owned process environments; the runner and every job run as the unprivileged `runner` user with a clean env.
set -euo pipefail

: "${GITHUB_REPOSITORY:?set GITHUB_REPOSITORY=owner/repo}"
: "${GITHUB_PAT:?set GITHUB_PAT}"
LABELS="${RUNNER_LABELS:-rs64,linux}"
NAME="${RUNNER_NAME:-rs64-linux-$(hostname)}"
API="https://api.github.com/repos/${GITHUB_REPOSITORY}/actions/runners"
RUNNER_DIR=/home/runner/actions-runner

for f in roguesquadron.elf factor5_ucode; do
    [ -e "${RS64_INPUTS}/$f" ] || { echo "entrypoint: ${RS64_INPUTS}/$f is not mounted" >&2; exit 1; }
done

token() {
    curl -fsSL -X POST -H "Authorization: Bearer ${GITHUB_PAT}" -H "Accept: application/vnd.github+json" "${API}/$1" | jq -r .token
}

as_runner() {
    runuser -u runner -- env -i HOME=/home/runner USER=runner PATH=/usr/local/bin:/usr/bin:/bin LANG=C.UTF-8 \
        RS64_INPUTS="$RS64_INPUTS" RS64_MIPS_GCC="$RS64_MIPS_GCC" RS64_MIPS_LD="$RS64_MIPS_LD" \
        bash -c "cd $RUNNER_DIR && $*"
}

as_runner ./config.sh --unattended --ephemeral --replace \
    --url "https://github.com/${GITHUB_REPOSITORY}" \
    --token "$(token registration-token)" \
    --name "$NAME" --labels "$LABELS" --work _work

deregister() {
    as_runner ./config.sh remove --token "$(token remove-token)" || true
}
trap 'deregister; exit 130' INT TERM

as_runner ./run.sh & wait $!
