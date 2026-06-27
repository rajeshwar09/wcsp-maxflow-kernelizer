#!/bin/bash
# ================================================================
# wcsp-maxflow-kernelizer — Colab clone-and-test script
#
# Clones YOUR latest pushed code from the private GitHub repo and
# runs whichever component's tests you ask for. Use this every time
# you've pushed new commits and want to verify them on a real GPU.
#
# WHY A TOKEN IS NEEDED:
#   Your repo (rajeshwar09/wcsp-maxflow-kernelizer) is private.
#   Colab has no saved GitHub login, so a Personal Access Token (PAT)
#   stands in for your password over HTTPS. The token is only kept
#   in this Colab session's memory — it is never written to a file
#   or committed anywhere.
#
# ONE-TIME SETUP (do this once, takes 2 minutes):
#   1. Go to: https://github.com/settings/tokens?type=beta
#   2. "Generate new token" (Fine-grained token)
#   3. Repository access -> Only select repositories ->
#      wcsp-maxflow-kernelizer
#   4. Permissions -> Repository permissions -> Contents -> Read-only
#      (read-only is enough; this script never pushes anything)
#   5. Generate, copy the token (starts with "github_pat_...")
#   6. Paste it when this script asks (input is hidden)
#
# USAGE ON COLAB:
#   1. Runtime -> Change runtime type -> T4 GPU
#   2. Upload this script, then in a cell:
#        !bash colab_topo_test.sh
#   3. Paste your token when prompted
#   4. Pick which component to test from the menu
#
# RE-RUNNING AFTER NEW COMMITS:
#   Just run the script again — it deletes any old clone and pulls
#   the latest main branch fresh, so it always tests what's actually
#   on GitHub right now, not a stale local copy.
# ================================================================

set -e

GITHUB_USER="rajeshwar09"
REPO_NAME="wcsp-maxflow-kernelizer"
CLONE_DIR="/content/${REPO_NAME}"

echo "================================================================"
echo " wcsp-maxflow-kernelizer — clone & test"
echo "================================================================"
echo ""

# ----------------------------------------------------------------
# Step 1: GPU check (fail fast if no GPU runtime)
# ----------------------------------------------------------------
echo "[1/5] checking GPU runtime..."
if ! nvidia-smi --query-gpu=name,driver_version,memory.total --format=csv,noheader 2>/dev/null; then
    echo "ERROR: no GPU found."
    echo "Fix: Runtime -> Change runtime type -> Hardware accelerator -> T4 GPU"
    exit 1
fi
echo ""

# ----------------------------------------------------------------
# Step 2: Get the token (hidden input, never echoed or saved to disk)
# ----------------------------------------------------------------
echo "[2/5] GitHub authentication"
echo "Repo is private: ${GITHUB_USER}/${REPO_NAME}"
read -s -p "Paste your GitHub Personal Access Token (input hidden): " GITHUB_TOKEN
echo ""
echo ""

if [ -z "$GITHUB_TOKEN" ]; then
    echo "ERROR: no token entered. A token is required for a private repo."
    exit 1
fi

# ----------------------------------------------------------------
# Step 3: Fresh clone of the latest main branch
#   - removes any previous clone so this always reflects the
#     CURRENT state of GitHub, not a cached copy
#   - the token is embedded only in the clone URL for this one
#     command and is not written to .git/config in plaintext
#     beyond what 'git clone' itself stores for the remote
# ----------------------------------------------------------------
echo "[3/5] cloning latest code from GitHub..."
rm -rf "$CLONE_DIR"

CLONE_URL="https://${GITHUB_USER}:${GITHUB_TOKEN}@github.com/${GITHUB_USER}/${REPO_NAME}.git"

if ! git clone --depth 1 "$CLONE_URL" "$CLONE_DIR" 2>&1 | grep -v "$GITHUB_TOKEN"; then
    echo "ERROR: clone failed. Check that:"
    echo "  - the token is valid and not expired"
    echo "  - the token has Contents: Read-only access to this repo"
    echo "  - the repo name/owner are correct: ${GITHUB_USER}/${REPO_NAME}"
    exit 1
fi

# Immediately scrub the token out of the stored remote URL so it
# doesn't linger in .git/config in plaintext for the rest of the session
cd "$CLONE_DIR"
git remote set-url origin "https://github.com/${GITHUB_USER}/${REPO_NAME}.git"
unset GITHUB_TOKEN
unset CLONE_URL

LATEST_COMMIT=$(git log -1 --format="%h %s (%ar)")
echo "cloned OK. Latest commit: $LATEST_COMMIT"
echo ""

# ----------------------------------------------------------------
# Step 4: Show what was actually pulled (so you can sanity check
#   this is the code you think you pushed)
# ----------------------------------------------------------------
echo "[4/5] repo contents pulled:"
find . -path ./.git -prune -o -type f -print | sort | sed 's/^/  /'
echo ""

# ----------------------------------------------------------------
# Step 5: Pick which component to test
# ----------------------------------------------------------------
echo "[5/5] which component do you want to test?"
echo "  1) CPU static max-flow"
echo "  2) GPU topology-driven max-flow"
echo "  3) GPU data-driven worklist max-flow (when it exists)"
echo "  4) All available components"
read -p "Enter choice [1-4]: " CHOICE
echo ""

run_dimacs_test() {
    local exe=$1
    local name=$2
    local file=$3
    local expected=$4

    echo "--- $name (expected max-flow = $expected) ---"
    "$exe" "$file"
    actual=$("$exe" "$file" 2>/dev/null | grep -m1 "max-flow" | grep -oE '[0-9]+$')
    if [ "$actual" = "$expected" ]; then
        echo "  PASS"
    else
        echo "  FAIL (got '$actual', expected '$expected')"
    fi
    echo ""
}

test_cpu_static() {
    echo "=========================================="
    echo " CPU static max-flow"
    echo "=========================================="
    if [ ! -f apps/maxflow_cpu_demo.cpp ]; then
        echo "SKIP: apps/maxflow_cpu_demo.cpp not found in repo yet."
        return
    fi
    g++ -std=c++17 -I. apps/maxflow_cpu_demo.cpp -o /tmp/maxflow_cpu_demo -O2
    run_dimacs_test /tmp/maxflow_cpu_demo "easy"   data/easy/easy.dimacs     5
    run_dimacs_test /tmp/maxflow_cpu_demo "medium" data/medium/medium.dimacs 23
    run_dimacs_test /tmp/maxflow_cpu_demo "hard"   data/hard/hard.dimacs     3
}

test_gpu_topology() {
    echo "=========================================="
    echo " GPU topology-driven max-flow"
    echo "=========================================="
    if [ ! -f apps/maxflow_gpu_topology_demo.cu ]; then
        echo "SKIP: apps/maxflow_gpu_topology_demo.cu not found in repo yet."
        return
    fi
    nvcc -std=c++17 -I. -O2 apps/maxflow_gpu_topology_demo.cu -o /tmp/maxflow_gpu_topology_demo \
        || nvcc -std=c++14 -I. -O2 apps/maxflow_gpu_topology_demo.cu -o /tmp/maxflow_gpu_topology_demo
    run_dimacs_test /tmp/maxflow_gpu_topology_demo "easy"   data/easy/easy.dimacs     5
    run_dimacs_test /tmp/maxflow_gpu_topology_demo "medium" data/medium/medium.dimacs 23
    run_dimacs_test /tmp/maxflow_gpu_topology_demo "hard"   data/hard/hard.dimacs     3
}

test_gpu_worklist() {
    echo "=========================================="
    echo " GPU data-driven worklist max-flow"
    echo "=========================================="
    if [ ! -f apps/maxflow_gpu_worklist_demo.cu ]; then
        echo "SKIP: apps/maxflow_gpu_worklist_demo.cu not found in repo yet."
        return
    fi
    nvcc -std=c++17 -I. -O2 apps/maxflow_gpu_worklist_demo.cu -o /tmp/maxflow_gpu_worklist_demo \
        || nvcc -std=c++14 -I. -O2 apps/maxflow_gpu_worklist_demo.cu -o /tmp/maxflow_gpu_worklist_demo
    run_dimacs_test /tmp/maxflow_gpu_worklist_demo "easy"   data/easy/easy.dimacs     5
    run_dimacs_test /tmp/maxflow_gpu_worklist_demo "medium" data/medium/medium.dimacs 23
    run_dimacs_test /tmp/maxflow_gpu_worklist_demo "hard"   data/hard/hard.dimacs     3
}

case "$CHOICE" in
    1) test_cpu_static ;;
    2) test_gpu_topology ;;
    3) test_gpu_worklist ;;
    4) test_cpu_static; test_gpu_topology; test_gpu_worklist ;;
    *) echo "invalid choice"; exit 1 ;;
esac

echo "================================================================"
echo " Done. Tested commit: $LATEST_COMMIT"
echo "================================================================"