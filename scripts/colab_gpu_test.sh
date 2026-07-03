#!/bin/bash
# ================================================================
# wcsp-maxflow-kernelizer — Colab clone-and-test script (PUBLIC REPO)
#
# Clones the latest code from the public GitHub repo and runs
# whichever component's tests
#
# USAGE ON COLAB:
#   1. Runtime -> Change runtime type -> T4 GPU
#   2. Upload this script, then in a cell:
#        !bash colab_gpu_test.sh
#   3. Pick which component to test from the menu
#
# RE-RUNNING AFTER NEW COMMITS:
#   Just run the script again — it deletes any old clone and pulls
#   the latest main branch fresh.
# ================================================================

set -e

GITHUB_USER="rajeshwar09"
REPO_NAME="wcsp-maxflow-kernelizer"
CLONE_DIR="/content/${REPO_NAME}"

echo "================================================================"
echo " wcsp-maxflow-kernelizer — clone & test (public repo)"
echo "================================================================"
echo ""

# ----------------------------------------------------------------
# Step 1: GPU check (fail fast if no GPU runtime)
# ----------------------------------------------------------------
echo "[1/4] checking GPU runtime..."
if ! nvidia-smi --query-gpu=name,driver_version,memory.total --format=csv,noheader 2>/dev/null; then
    echo "ERROR: no GPU found."
    echo "Fix: Runtime -> Change runtime type -> Hardware accelerator -> T4 GPU"
    exit 1
fi
echo ""

# ----------------------------------------------------------------
# Step 2: Fresh clone of the latest main branch
# ----------------------------------------------------------------
echo "[2/4] cloning latest code from GitHub (public)..."
rm -rf "$CLONE_DIR"

if ! git clone --depth 1 "https://github.com/${GITHUB_USER}/${REPO_NAME}.git" "$CLONE_DIR" 2>&1; then
    echo "ERROR: clone failed. Check repo name: ${GITHUB_USER}/${REPO_NAME}"
    exit 1
fi

cd "$CLONE_DIR"
LATEST_COMMIT=$(git log -1 --format="%h %s (%ar)")
echo "cloned OK. Latest commit: $LATEST_COMMIT"
echo ""

# ----------------------------------------------------------------
# Step 3: Show what was actually pulled
# ----------------------------------------------------------------
echo "[3/4] repo contents pulled:"
find . -path ./.git -prune -o -type f -print | sort | sed 's/^/  /'
echo ""

# ----------------------------------------------------------------
# Step 4: Pick which component to test
# ----------------------------------------------------------------
echo "[4/4] which component do you want to test?"
echo "  1) CPU static max-flow"
echo "  2) GPU topology-driven max-flow"
echo "  3) GPU data-driven worklist max-flow"
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
    nvcc -std=c++17 -arch=sm_75 -I. -O2 apps/maxflow_gpu_topology_demo.cu -o /tmp/maxflow_gpu_topology_demo \
        || nvcc -std=c++14 -arch=sm_75 -I. -O2 apps/maxflow_gpu_topology_demo.cu -o /tmp/maxflow_gpu_topology_demo
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
    nvcc -std=c++17 -arch=sm_75 -I. -O2 apps/maxflow_gpu_worklist_demo.cu -o /tmp/maxflow_gpu_worklist_demo \
        || nvcc -std=c++14 -arch=sm_75 -I. -O2 apps/maxflow_gpu_worklist_demo.cu -o /tmp/maxflow_gpu_worklist_demo
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