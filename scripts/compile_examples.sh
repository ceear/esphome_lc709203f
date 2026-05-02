#!/usr/bin/env bash
# Compile all ESPHome YAML examples to verify the component builds correctly.
# Run from the repository root: ./scripts/compile_examples.sh
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VENV_DIR="${REPO_ROOT}/.venv"
SECRETS_FILE="${REPO_ROOT}/examples/secrets.yaml"

echo "=== LC709203F Deep Sleep – ESPHome compile check ==="
echo "Repository root: ${REPO_ROOT}"

# ── Python virtual environment ────────────────────────────────────────────────
if [[ ! -d "${VENV_DIR}" ]]; then
  echo "Creating Python virtual environment at ${VENV_DIR}..."
  python3 -m venv "${VENV_DIR}"
fi

# shellcheck disable=SC1091
source "${VENV_DIR}/bin/activate"

# ── Install / upgrade ESPHome ─────────────────────────────────────────────────
echo "Installing / upgrading ESPHome..."
pip install --quiet --upgrade esphome

ESPHOME_VERSION="$(esphome version 2>&1 | head -n1)"
echo "ESPHome version: ${ESPHOME_VERSION}"

# ── Dummy secrets file ────────────────────────────────────────────────────────
# These values are only used during compilation (syntax check + code-gen).
# They are never flashed to a real device.
cat > "${SECRETS_FILE}" << 'EOF'
wifi_ssid: "TestNetwork"
wifi_password: "testpassword"
mqtt_broker: "192.168.1.1"
mqtt_username: "esphome"
mqtt_password: "esphome"
ota_password: "testota"
api_encryption_key: "AQIDBAUGBwgJCgsMDQ4PEBESExQVFhcYGRobHB0eHyA="
EOF
echo "Dummy secrets.yaml written to ${SECRETS_FILE}"

# ── Compile each example ───────────────────────────────────────────────────────
EXAMPLES=(
  "examples/esp32-deep-sleep.yaml"
  "examples/esp32-normal.yaml"
  "examples/esp32-debug.yaml"
)

PASS=0
FAIL=0

for yaml in "${EXAMPLES[@]}"; do
  abs_yaml="${REPO_ROOT}/${yaml}"
  echo ""
  echo "── Compiling ${yaml} ──"
  if esphome compile "${abs_yaml}"; then
    echo "  ✓ PASS: ${yaml}"
    PASS=$((PASS + 1))
  else
    echo "  ✗ FAIL: ${yaml}"
    FAIL=$((FAIL + 1))
  fi
done

# ── Summary ───────────────────────────────────────────────────────────────────
echo ""
echo "=== Compile summary ==="
echo "  ESPHome: ${ESPHOME_VERSION}"
echo "  Passed:  ${PASS}"
echo "  Failed:  ${FAIL}"

if [[ ${FAIL} -gt 0 ]]; then
  echo "  RESULT: FAIL"
  exit 1
else
  echo "  RESULT: PASS"
  exit 0
fi
