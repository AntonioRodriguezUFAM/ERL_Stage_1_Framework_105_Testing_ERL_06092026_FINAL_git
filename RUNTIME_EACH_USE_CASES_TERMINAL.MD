#!/usr/bin/env bash
set -e

# Benchmark duration in seconds (set to 100 for quick testing, 1800 for full runs)
DURATION=1800

mkdir -p benchmark_logs

# ==============================================================================
# 1. SobelEdge Benchmark Suite
# ==============================================================================
echo "--- Starting SobelEdge Suite ---"
sudo ./MySystem --algo SobelEdge --mode CPU_STATIC ${DURATION} config.json | tee benchmark_logs/SobelEdge_CPU_STATIC.log
sudo ./MySystem --algo SobelEdge --mode GPU_STATIC ${DURATION} config.json | tee benchmark_logs/SobelEdge_GPU_STATIC.log
sudo ./MySystem --algo SobelEdge --mode BALANCED ${DURATION} config.json | tee benchmark_logs/SobelEdge_BALANCED.log
sudo ./MySystem --algo SobelEdge --mode ERL ${DURATION} config.json | tee benchmark_logs/SobelEdge_ERL.log
# Temperature Sweep Cases
sudo ./MySystem --algo SobelEdge --mode ERL --temp-offset -10 ${DURATION} config.json | tee benchmark_logs/SobelEdge_ERL_TempOff_neg10C.log
sudo ./MySystem --algo SobelEdge --mode ERL --temp-offset 10 ${DURATION} config.json | tee benchmark_logs/SobelEdge_ERL_TempOff_10C.log

# ==============================================================================
# 2. HistogramEqualization Benchmark Suite
# ==============================================================================
echo "--- Starting HistogramEqualization Suite ---"
sudo ./MySystem --algo HistogramEqualization --mode CPU_STATIC ${DURATION} config.json | tee benchmark_logs/HistEq_CPU_STATIC.log
sudo ./MySystem --algo HistogramEqualization --mode GPU_STATIC ${DURATION} config.json | tee benchmark_logs/HistEq_GPU_STATIC.log
sudo ./MySystem --algo HistogramEqualization --mode BALANCED ${DURATION} config.json | tee benchmark_logs/HistEq_BALANCED.log
sudo ./MySystem --algo HistogramEqualization --mode ERL ${DURATION} config.json | tee benchmark_logs/HistEq_ERL.log
# Temperature Sweep Cases
sudo ./MySystem --algo HistogramEqualization --mode ERL --temp-offset -10 ${DURATION} config.json | tee benchmark_logs/HistEq_ERL_TempOff_neg10C.log
sudo ./MySystem --algo HistogramEqualization --mode ERL --temp-offset 10 ${DURATION} config.json | tee benchmark_logs/HistEq_ERL_TempOff_10C.log

# ==============================================================================
# 3. HeterogeneousGaussianBlur Benchmark Suite
# ==============================================================================
echo "--- Starting HeterogeneousGaussianBlur Suite ---"
sudo ./MySystem --algo HeterogeneousGaussianBlur --mode CPU_STATIC ${DURATION} config.json | tee benchmark_logs/HetGBlur_CPU_STATIC.log
sudo ./MySystem --algo HeterogeneousGaussianBlur --mode GPU_STATIC ${DURATION} config.json | tee benchmark_logs/HetGBlur_GPU_STATIC.log
sudo ./MySystem --algo HeterogeneousGaussianBlur --mode BALANCED ${DURATION} config.json | tee benchmark_logs/HetGBlur_BALANCED.log
sudo ./MySystem --algo HeterogeneousGaussianBlur --mode ERL ${DURATION} config.json | tee benchmark_logs/HetGBlur_ERL.log
# Temperature Sweep Cases
sudo ./MySystem --algo HeterogeneousGaussianBlur --mode ERL --temp-offset -10 ${DURATION} config.json | tee benchmark_logs/HetGBlur_ERL_TempOff_neg10C.log
sudo ./MySystem --algo HeterogeneousGaussianBlur --mode ERL --temp-offset 10 ${DURATION} config.json | tee benchmark_logs/HetGBlur_ERL_TempOff_10C.log

# ==============================================================================
# 4. GaussianBlur Benchmark Suite
# ==============================================================================
echo "--- Starting GaussianBlur Suite ---"
sudo ./MySystem --algo GaussianBlur --mode CPU_STATIC ${DURATION} config.json | tee benchmark_logs/GBlur_CPU_STATIC.log
sudo ./MySystem --algo GaussianBlur --mode GPU_STATIC ${DURATION} config.json | tee benchmark_logs/GBlur_GPU_STATIC.log
sudo ./MySystem --algo GaussianBlur --mode BALANCED ${DURATION} config.json | tee benchmark_logs/GBlur_BALANCED.log
sudo ./MySystem --algo GaussianBlur --mode ERL ${DURATION} config.json | tee benchmark_logs/GBlur_ERL.log
# Temperature Sweep Cases
sudo ./MySystem --algo GaussianBlur --mode ERL --temp-offset -10 ${DURATION} config.json | tee benchmark_logs/GBlur_ERL_TempOff_neg10C.log
sudo ./MySystem --algo GaussianBlur --mode ERL --temp-offset 10 ${DURATION} config.json | tee benchmark_logs/GBlur_ERL_TempOff_10C.log

# ==============================================================================
# 5. MedianFilter Benchmark Suite (Strict GPU-Bound test)
# ==============================================================================
echo "--- Starting MedianFilter Suite ---"
sudo ./MySystem --algo MedianFilter --mode CPU_STATIC ${DURATION} config.json | tee benchmark_logs/MedFilter_CPU_STATIC.log
sudo ./MySystem --algo MedianFilter --mode GPU_STATIC ${DURATION} config.json | tee benchmark_logs/MedFilter_GPU_STATIC.log
sudo ./MySystem --algo MedianFilter --mode BALANCED ${DURATION} config.json | tee benchmark_logs/MedFilter_BALANCED.log
sudo ./MySystem --algo MedianFilter --mode ERL ${DURATION} config.json | tee benchmark_logs/MedFilter_ERL.log
# Temperature Sweep Cases
sudo ./MySystem --algo MedianFilter --mode ERL --temp-offset -10 ${DURATION} config.json | tee benchmark_logs/MedFilter_ERL_TempOff_neg10C.log
sudo ./MySystem --algo MedianFilter --mode ERL --temp-offset 10 ${DURATION} config.json | tee benchmark_logs/MedFilter_ERL_TempOff_10C.log

echo "--- All Benchmark Suites Completed Successfully ---"


Here are two ways to handle this. You can either use an interactive wrapper script that pauses between test cases so you can fix/reset USB devices without breaking the entire run, or run the individual commands directly one by one.

---

## Option 1: Interactive Terminal Runner (Recommended for USB Bugs)

This script runs **one test at a time** and prompts you before moving to the next. If a USB device drops or resets during a run, you can fix the hardware, press `r` to retry the exact same test, or press `ENTER` to continue to the next one.

Save this as `run_interactive.sh`, make it executable with `chmod +x run_interactive.sh`, and run it with `sudo ./run_interactive.sh`:

```bash
#!/usr/bin/env bash

# Set duration (1800 for full benchmark, 100 for fast tests)
DURATION=1800

mkdir -p benchmark_logs

# Define all benchmark cases
TESTS=(
    # SobelEdge
    "SobelEdge CPU_STATIC"
    "SobelEdge GPU_STATIC"
    "SobelEdge BALANCED"
    "SobelEdge ERL"
    "SobelEdge ERL -10"
    "SobelEdge 10"

    # HistogramEqualization
    "HistogramEqualization CPU_STATIC"
    "HistogramEqualization GPU_STATIC"
    "HistogramEqualization BALANCED"
    "HistogramEqualization ERL"
    "HistogramEqualization ERL -10"
    "HistogramEqualization ERL 10"

    # HeterogeneousGaussianBlur
    "HeterogeneousGaussianBlur CPU_STATIC"
    "HeterogeneousGaussianBlur GPU_STATIC"
    "HeterogeneousGaussianBlur BALANCED"
    "HeterogeneousGaussianBlur ERL"
    "HeterogeneousGaussianBlur ERL -10"
    "HeterogeneousGaussianBlur ERL 10"

    # GaussianBlur
    "GaussianBlur CPU_STATIC"
    "GaussianBlur GPU_STATIC"
    "GaussianBlur BALANCED"
    "GaussianBlur ERL"
    "GaussianBlur ERL -10"
    "GaussianBlur ERL 10"

    # MedianFilter
    "MedianFilter CPU_STATIC"
    "MedianFilter GPU_STATIC"
    "MedianFilter BALANCED"
    "MedianFilter ERL"
    "MedianFilter ERL -10"
    "MedianFilter ERL 10"
)

for entry in "${TESTS[@]}"; do
    read -r ALGO MODE OFFSET <<< "$entry"

    # Build command and output log name
    if [ -n "$OFFSET" ]; then
        if [ "$OFFSET" == "-10" ]; then
            LOG_NAME="${ALGO}_${MODE}_TempOff_neg10C.log"
        else
            LOG_NAME="${ALGO}_${MODE}_TempOff_10C.log"
        fi
        CMD="./MySystem --algo $ALGO --mode $MODE --temp-offset $OFFSET $DURATION config.json"
    else
        LOG_NAME="${ALGO}_${MODE}.log"
        CMD="./MySystem --algo $ALGO --mode $MODE $DURATION config.json"
    fi

    while true; do
        echo ""
        echo "========================================================================"
        echo " READY TO RUN: $CMD"
        echo " LOG FILE:     benchmark_logs/$LOG_NAME"
        echo "========================================================================"
        read -p "Press [ENTER] to start, [s] to skip, or [q] to quit: " choice

        case "$choice" in
            q|Q) echo "Exiting benchmark suite."; exit 0 ;;
            s|S) echo "Skipping test..."; break ;;
            *)
                echo "Running test..."
                eval "$CMD | tee benchmark_logs/$LOG_NAME"
                
                read -p "Test finished. Press [ENTER] to move on, or [r] to retry this test: " retry
                if [[ "$retry" != "r" && "$retry" != "R" ]]; then
                    break
                fi
                ;;
        esac
    done
done

```

---

## Option 2: Individual Terminal Commands

If you prefer to manually copy and paste each command directly into your terminal one at a time, use the list below.

### 1. SobelEdge Suite

```bash
sudo ./MySystem --algo SobelEdge --mode CPU_STATIC 1800 config.json | tee benchmark_logs/SobelEdge_CPU_STATIC.log

sudo ./MySystem --algo SobelEdge --mode GPU_STATIC 1800 config.json | tee benchmark_logs/SobelEdge_GPU_STATIC.log

sudo ./MySystem --algo SobelEdge --mode BALANCED 1800 config.json | tee benchmark_logs/SobelEdge_BALANCED.log

sudo ./MySystem --algo SobelEdge --mode ERL 1800 config.json | tee benchmark_logs/SobelEdge_ERL.log

sudo ./MySystem --algo SobelEdge --mode ERL --temp-offset -10 1800 config.json | tee benchmark_logs/SobelEdge_ERL_TempOff_neg10C.log

sudo ./MySystem --algo SobelEdge --mode ERL --temp-offset 10 1800 config.json | tee benchmark_logs/SobelEdge_ERL_TempOff_10C.log

```

### 2. HistogramEqualization Suite

```bash
sudo ./MySystem --algo HistogramEqualization --mode CPU_STATIC 1800 config.json | tee benchmark_logs/HistEq_CPU_STATIC.log

sudo ./MySystem --algo HistogramEqualization --mode GPU_STATIC 1800 config.json | tee benchmark_logs/HistEq_GPU_STATIC.log

sudo ./MySystem --algo HistogramEqualization --mode BALANCED 1800 config.json | tee benchmark_logs/HistEq_BALANCED.log

sudo ./MySystem --algo HistogramEqualization --mode ERL 1800 config.json | tee benchmark_logs/HistEq_ERL.log

sudo ./MySystem --algo HistogramEqualization --mode ERL --temp-offset -10 1800 config.json | tee benchmark_logs/HistEq_ERL_TempOff_neg10C.log

sudo ./MySystem --algo HistogramEqualization --mode ERL --temp-offset 10 1800 config.json | tee benchmark_logs/HistEq_ERL_TempOff_10C.log

```

### 3. HeterogeneousGaussianBlur Suite

```bash
sudo ./MySystem --algo HeterogeneousGaussianBlur --mode CPU_STATIC 1800 config.json | tee benchmark_logs/HetGBlur_CPU_STATIC.log

sudo ./MySystem --algo HeterogeneousGaussianBlur --mode GPU_STATIC 1800 config.json | tee benchmark_logs/HetGBlur_GPU_STATIC.log

sudo ./MySystem --algo HeterogeneousGaussianBlur --mode BALANCED 1800 config.json | tee benchmark_logs/HetGBlur_BALANCED.log

sudo ./MySystem --algo HeterogeneousGaussianBlur --mode ERL 1800 config.json | tee benchmark_logs/HetGBlur_ERL.log

sudo ./MySystem --algo HeterogeneousGaussianBlur --mode ERL --temp-offset -10 1800 config.json | tee benchmark_logs/HetGBlur_ERL_TempOff_neg10C.log

sudo ./MySystem --algo HeterogeneousGaussianBlur --mode ERL --temp-offset 10 1800 config.json | tee benchmark_logs/HetGBlur_ERL_TempOff_10C.log

```

### 4. GaussianBlur Suite

```bash
sudo ./MySystem --algo GaussianBlur --mode CPU_STATIC 1800 config.json | tee benchmark_logs/GBlur_CPU_STATIC.log

sudo ./MySystem --algo GaussianBlur --mode GPU_STATIC 1800 config.json | tee benchmark_logs/GBlur_GPU_STATIC.log

sudo ./MySystem --algo GaussianBlur --mode BALANCED 1800 config.json | tee benchmark_logs/GBlur_BALANCED.log

sudo ./MySystem --algo GaussianBlur --mode ERL 1800 config.json | tee benchmark_logs/GBlur_ERL.log

sudo ./MySystem --algo GaussianBlur --mode ERL --temp-offset -10 1800 config.json | tee benchmark_logs/GBlur_ERL_TempOff_neg10C.log

sudo ./MySystem --algo GaussianBlur --mode ERL --temp-offset 10 1800 config.json | tee benchmark_logs/GBlur_ERL_TempOff_10C.log

```

### 5. MedianFilter Suite

```bash
sudo ./MySystem --algo MedianFilter --mode CPU_STATIC 1800 config.json | tee benchmark_logs/MedFilter_CPU_STATIC.log

sudo ./MySystem --algo MedianFilter --mode GPU_STATIC 1800 config.json | tee benchmark_logs/MedFilter_GPU_STATIC.log

sudo ./MySystem --algo MedianFilter --mode BALANCED 1800 config.json | tee benchmark_logs/MedFilter_BALANCED.log

sudo ./MySystem --algo MedianFilter --mode ERL 1800 config.json | tee benchmark_logs/MedFilter_ERL.log

sudo ./MySystem --algo MedianFilter --mode ERL --temp-offset -10 1800 config.json | tee benchmark_logs/MedFilter_ERL_TempOff_neg10C.log

sudo ./MySystem --algo MedianFilter --mode ERL --temp-offset 10 1800 config.json | tee benchmark_logs/MedFilter_ERL_TempOff_10C.log

```