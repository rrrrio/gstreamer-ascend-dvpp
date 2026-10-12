#!/usr/bin/env bash
set -euo pipefail

# Compare the same multi-branch conversion workload on host CPU and Ascend DVPP.
# The input file is decoded once before tee, so decode cost is common to both runs.

mode=${1:-dvpp}
input_file=${2:-test.h264}
branches=${BRANCHES:-6}
duration=${DURATION:-20}
device_id=${DEVICE_ID:-0}
inflight_depth=${INFLIGHT_DEPTH:-8}
stream_count=${STREAM_COUNT:-2}
npu_id=${NPU_ID:-64}
npu_core=${NPU_CORE:-0}

if [[ "$mode" != cpu && "$mode" != dvpp ]]; then
  echo "usage: $0 cpu|dvpp [h264-file]" >&2
  exit 2
fi
if [[ ! -r "$input_file" ]]; then
  echo "input file is not readable: $input_file" >&2
  exit 2
fi
if ! command -v gst-launch-1.0 >/dev/null; then
  echo "gst-launch-1.0 is required" >&2
  exit 2
fi

if [[ "$mode" == dvpp ]]; then
  : "${GST_PLUGIN_PATH_1_0:?set GST_PLUGIN_PATH_1_0 and source the CANN environment first}"
fi

tmp_dir=$(mktemp -d /tmp/gst-ascend-cpu-vs-dvpp.XXXXXX)
samples="$tmp_dir/samples"
touch "$samples"
pipeline_pid=""
cleanup() {
  if [[ -n "$pipeline_pid" ]] && kill -0 "$pipeline_pid" 2>/dev/null; then
    kill "$pipeline_pid" 2>/dev/null || true
    wait "$pipeline_pid" 2>/dev/null || true
  fi
  rm -rf "$tmp_dir"
}
trap cleanup EXIT INT TERM

args=(filesrc location="$input_file" ! h264parse ! avdec_h264 ! videoconvert
      ! "video/x-raw,format=NV12,width=1920,height=1080" ! tee name=t)
for ((i = 0; i < branches; i++)); do
  args+=(t. ! queue !)
  if [[ "$mode" == cpu ]]; then
    args+=(videoscale ! "video/x-raw,width=640,height=480" ! videoconvert
           ! "video/x-raw,format=BGR" ! fakesink sync=false)
  else
    args+=(ascendvideoconvert device-id="$device_id"
           inflight-depth="$inflight_depth" stream-count="$stream_count"
           ! "video/x-raw,width=640,height=480,format=BGR"
           ! fakesink sync=false)
  fi
done

tracer_args=()
if [[ "${GST_SHARK_CPUUSAGE:-0}" == 1 ]] &&
   gst-inspect-1.0 cpuusage >/dev/null 2>&1; then
  tracer_args=(GST_DEBUG=GST_TRACER:7 GST_TRACERS=cpuusage)
fi

env "${tracer_args[@]}" gst-launch-1.0 -q "${args[@]}" &
pipeline_pid=$!

for ((elapsed = 0; elapsed < duration; elapsed++)); do
  if ! kill -0 "$pipeline_pid" 2>/dev/null; then
    break
  fi
  host_cpu=$(ps -p "$pipeline_pid" -o pcpu= | awk '{print $1 + 0}')
  npu_values=$(npu-smi info -t usages -i "$npu_id" -c "$npu_core" 2>/dev/null |
    awk -F: '
      /DDR Usage Rate/ {ddr=$2 + 0}
      /Aicpu Usage Rate/ {aicpu=$2 + 0}
      /Ctrlcpu Usage Rate/ {ctrlcpu=$2 + 0}
      /DVPP VPC Usage Rate/ {vpc=$2 + 0}
      END {printf "%d %d %d %d", ddr, aicpu, ctrlcpu, vpc}')
  [[ -n "$npu_values" ]] || npu_values="-1 -1 -1 -1"
  printf "%s %s\n" "$host_cpu" "$npu_values" >> "$samples"
  sleep 1
done
if kill -0 "$pipeline_pid" 2>/dev/null; then
  kill "$pipeline_pid" 2>/dev/null || true
fi
wait "$pipeline_pid" 2>/dev/null || true
pipeline_pid=""

awk -v mode="$mode" -v branches="$branches" '
  { host += $1; ddr += $2; aicpu += $3; ctrlcpu += $4; vpc += $5; n++; if ($2 >= 0) npu_available++ }
  END {
    if (n == 0) { print "no samples collected" > "/dev/stderr"; exit 1 }
    printf "mode=%s branches=%d samples=%d host_cpu_avg=%.1f%% DDR_avg=%.1f%% ", mode, branches, n, host / n, ddr / n
    if (npu_available == 0) printf "AICPU_avg=unavailable CtrlCPU_avg=unavailable DVPP_VPC_avg=unavailable\n"
    else printf "AICPU_avg=%.1f%% CtrlCPU_avg=%.1f%% DVPP_VPC_avg=%.1f%%\n", aicpu / n, ctrlcpu / n, vpc / n
  }' "$samples"
