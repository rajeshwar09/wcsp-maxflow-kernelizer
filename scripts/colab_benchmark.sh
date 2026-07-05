#!/bin/bash
# ================================================================
# Colab benchmark: CPU vs GPU max-flow on bipartite double-cover
# ================================================================

set -e

GITHUB_USER="rajeshwar09"
REPO_NAME="wcsp-maxflow-kernelizer"
CLONE_DIR="/content/${REPO_NAME}"

echo "================================================================"
echo " Max-Flow Kernelizer Benchmark (public repo)"
echo "================================================================"
echo ""

# GPU check
echo "[1/3] checking GPU runtime..."
nvidia-smi --query-gpu=name,driver_version,memory.total --format=csv,noheader 2>/dev/null || {
    echo "ERROR: no GPU. Use Runtime -> Change runtime type -> T4 GPU"
    exit 1
}
echo ""

# Clone
echo "[2/3] cloning latest code..."
rm -rf "$CLONE_DIR"
git clone --depth 1 "https://github.com/${GITHUB_USER}/${REPO_NAME}.git" "$CLONE_DIR" 2>&1
cd "$CLONE_DIR"
echo "Latest commit: $(git log -1 --format='%h %s (%ar)')"
echo ""

# Build and run benchmark
echo "[3/3] building and running benchmark..."
echo ""
nvcc -std=c++17 -arch=sm_75 -I. -O2 apps/benchmark_gpu.cu -o /tmp/benchmark_gpu
echo ""
/tmp/benchmark_gpu

echo ""
echo "================================================================"
echo " Benchmark complete"
echo "================================================================"