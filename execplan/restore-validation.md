# Restore and Validate Ascend DVPP Work

This document is a living execution plan for restoring this repository after a
long pause. It follows `execplan/references/PLANS.md` from the Codex skill
instructions. The goal is to turn the experimental plugin into a state whose
implemented behavior, tests, hardware evidence, and documentation agree.

## Purpose / Big Picture

After this work, a contributor can build the plugin in both mock and CANN modes,
run a meaningful host-only validation suite, and identify exactly which parts
have been exercised on an Ascend device. VPC and JPEG must remain backed by
real evidence. VDEC/VENC must have a working report-processing path and a
reproducible record of which physical codec streams have actually passed.

## Progress

- [x] (2026-10-12 00:00 +0800) Re-read the repository history, design notes,
  source layout, and dirty worktree.
- [x] (2026-10-12 00:00 +0800) Confirmed the mock build, one existing test,
  plugin loading, seven factory registrations, and a short VPC pipeline.
- [x] (2026-10-12 00:00 +0800) Found the local CANN 9.1.0-beta.1 headers and
  libraries.
- [x] (2026-10-12 19:30 +0800) Fixed the hardware test target so it links the
  CANN libraries when `with_ascend=true`.
- [x] (2026-10-12 19:35 +0800) Added allocator sharing, per-device allocator,
  all-element factory, and plugin-load tests; the mock suite reports 4/4 tests
  passing.
- [x] (2026-10-12 19:36 +0800) Built the CANN mode and linked all test targets.
  With no visible Ascend device, runtime tests are not registered at configure
  time; the CANN plugin-load test still passes.
- [x] (2026-10-12 19:50 +0800) Implemented one report worker per VDEC/VENC
  element, bound its OS thread ID before channel creation, and compiled it
  against the installed CANN 9.1.0-beta.1 headers and libraries.
- [x] (2026-10-12 20:05 +0800) Added chained-converter device-memory reuse and
  real FLUSH cleanup tests. The mock suite now exercises five converter cases
  and the full Meson suite reports 4/4 passing.
- [x] (2026-10-12 20:20 +0800) Closed the multi-device runtime release gap:
  all devices activated during a plugin-owned ACL lifetime are reset before
  finalization, while `ACL_ERROR_REPEAT_INITIALIZE` no longer causes the
  plugin to finalize an externally owned runtime. Mock tests now cover runtime
  reference counting and codec start/stop lifecycle.
- [x] (2026-10-12 20:40 +0800) Confirmed the physical target through privileged
  device access: CANN 9.1.0-beta.1, product `IT21PD2G10`, and
  `/dev/davinci0`, `/dev/davinci1`, `/dev/devmm_svm`.
- [x] (2026-10-12 20:45 +0800) Validated the CANN suite on hardware: allocator,
  VPC including chained 128x96 -> 64x48 -> 32x24 reuse, element lifecycle, and
  plugin loading all pass 4/4. JPEG 320x240 encode/decode also reaches EOS.
- [x] (2026-10-12 21:00 +0800) Validated the H.264 report path on hardware:
  four-frame VENC, four-access-unit VDEC, EOS drain, and channel teardown all
  exit successfully. H.265 was still unverified at this checkpoint.
- [x] (2026-10-12 21:10 +0800) Synchronized the English and Chinese README,
  design notes, hardware matrix, and this execution plan with the evidence.
- [x] (2026-10-12 21:25 +0800) Fixed the H.264/H.265 encoder factory type split
  so the H.265 encoder retains its codec selection after raw-input negotiation.
  On `IT21PD2G10`, four 320x240 NV12 frames produced a stream independently
  identified as H.265/HEVC, and `h265parse ! ascendh265dec` reached EOS with
  clean channel teardown.
- [x] (2026-10-12 21:25 +0800) Updated the README files, design notes, and
  hardware matrix to record the H.265 evidence while keeping broader profiles
  and rate-control combinations unverified.
- [ ] Create the first repository commit after the configured pre-commit
  security hooks are runnable in this environment.

## Surprises & Discoveries

- Observation: The mock build passed, but the first hardware build failed while
  linking only `test-ascend-memory`.
  Evidence: The test target referenced `aclInit`, `acldvppMalloc`, and related
  symbols without linking `ascend_lib`; the plugin target itself compiled.
- Observation: CANN headers and libraries are installed locally, but shell
  expansion did not find `/dev/davinci*` and no device listing was observed.
  Evidence: CANN files exist under `/usr/local/Ascend/cann-9.1.0-beta.1`,
  `npu-smi info -l` timed out after five seconds, and the direct `find /dev`
  probe initially returned no `davinci*` or `devmm_svm` node in the restricted
  shell. A later privileged read-only probe found both device nodes and
  `npu-smi info -l` reported product `IT21PD2G10`.
- Observation: Hardware-mode compilation now succeeds, but runtime tests need
  a device and are omitted from the test plan when no device node is visible.
  Evidence: `ninja` completed the CANN plugin and four test executables;
  `meson test` runs the plugin-load test and reports no runtime tests when the
  configure-time `/dev` probe finds no Ascend node.
- Observation: The VDEC/VENC code registers callbacks and waits on condition
  variables, but initially had no report-processing worker.
  Evidence: the restored source now creates one worker per codec element,
  binds its TID with `aclvdec/aclvencSetChannelDescThreadId`, and compiles the
  `aclrtProcessReport` loop in CANN mode. The installed CANN documentation says
  codec channel creation owns the internal stream subscription.
- Observation: A converter chain can deadlock even when a single converter
  reaches EOS if its output pool only reserves `inflight-depth + 2` buffers.
  Evidence: the new two-converter mock pipeline stalled after two downstream
  buffers; reserving `2 * inflight-depth + 4` buffers made the same pipeline
  reach EOS and pass the device-memory probe.
- Observation: The first physical-device chain probe used a 16x12 final
  output, which the current device rejected; the same device-memory reuse path
  reached EOS with 128x96 -> 64x48 -> 32x24 NV12 stages. The test now uses the
  supported dimensions and records the smaller-size limit as unverified beyond
  this product.
- Observation: VDEC callbacks can arrive after multiple `handle_frame` calls
  and the final frame may not be emitted until the channel receives an EOS
  descriptor. The implementation therefore queues callback results during
  `finish`, completes frames on the GStreamer EOS thread, and destroys the
  channel while the report worker is still alive.
- Observation: VENC needed an explicit ACL report stream subscription on this
  CANN target. The encoder now creates, subscribes, unsubscribes, and destroys
  that stream as part of its channel lifecycle.

## Decision Log

- Decision: Preserve the existing VPC allocator and in-flight implementation as
  the first restoration batch, then add tests around its behavior.
  Rationale: It is already represented in the dirty worktree and has a known
  mock smoke path; splitting it first makes later codec work easier to review.
  Date/Author: 2026-10-12 / Yuhao Ding.
- Decision: Treat mock success, CANN link success, and physical-device success as
  three separate validation levels.
  Rationale: A host allocation path cannot prove device pointers, DVPP output,
  callbacks, or device occupancy.
  Date/Author: 2026-10-12 / Yuhao Ding.
- Decision: Do not invent a report-loop abstraction until the installed CANN
  callback contract and device availability are confirmed.
  Rationale: CANN 9.1 exposes both DVPP callback thread IDs and ACL report
  subscription APIs; choosing the wrong ownership model could deadlock or leak
  codec frames.
  Date/Author: 2026-10-12 / Yuhao Ding.
- Decision: Use one report worker per codec element. Let VDEC use its channel
  report path, while VENC owns an explicit report stream subscription.
  Rationale: CANN 9.1 binds callbacks to a user thread through the channel
  description, but the physical VENC path required an explicit
  `aclrtSubscribeReport` stream. A per-element worker also avoids reusing one
  thread ID across devices.
  Date/Author: 2026-10-12 / Yuhao Ding.

## Outcomes & Retrospective

The restoration now has both host-only and physical evidence. Mock and CANN
Meson suites report 4/4 passing; physical VPC, JPEG, H.264 VENC/VDEC, and
H.265 VENC/VDEC smoke pipelines reach EOS on the current target. Broader codec
profiles and performance saturation remain outside the evidence, so the plugin
is still experimental rather than production-ready.

## Context and Orientation

`src/gstascendruntime.c` owns shared ACL initialization, per-thread device
activation, DVPP allocation, host/device copies, and picture descriptors.
`src/gstascendmemory.c` exposes `AscendDVPP` as a GStreamer allocator.
`src/gstascendvideoconvert.c` owns VPC caps, buffer allocation, asynchronous
jobs, stream retirement, EOS, and flush handling. `src/gstascendjpeg.c` contains
the synchronous JPEGD/JPEGE transforms. `src/gstascendvideocodec.c` contains
the VDEC/VENC GStreamer elements, report workers, VDEC pending-frame queue,
and VENC callback wait context.
`meson.build` selects mock or CANN mode with `-Dwith_ascend`.

The relevant ownership boundary is:

    system buffer --map/copy--> DVPP allocation --ACL DVPP--> output buffer
                                      ^                         |
                                      |                         v
                               pending job owns descriptors and references

In CANN mode, a callback/report mechanism may outlive the function that submits
a codec frame. Any implementation must keep input allocations, output buffers,
descriptors, and wait state alive until completion or cancellation.

## Plan of Work

The VPC allocator/in-flight batch and codec lifecycle work are now validated in
both mock and CANN builds. The host-only suite exercises negotiation, different
depths, chained device-memory reuse, restart, and flush cleanup. The physical
record covers VPC, JPEG, and bounded H.264/H.265 smoke streams; capture only
measurements that actually ran and keep broader codec combinations outside the
claims.

Every behavior change must update `DESIGN.md`, `README.md`, and
`README.zh-CN.md` when it changes user-visible status or commands. Hardware
constraints belong in `docs/hardware-matrix.md`.

## Concrete Steps

Run from `/workspace/VisionSDK/gstreamer-ascend-dvpp`.

    meson setup /tmp/gst-ascend-dvpp-build -Dwith_ascend=false -Dtests=true
    ninja -C /tmp/gst-ascend-dvpp-build
    meson test -C /tmp/gst-ascend-dvpp-build --print-errorlogs

    meson setup /tmp/gst-ascend-dvpp-build-ascend \
      -Dwith_ascend=true -Dtests=true \
      -Dascend_prefix=/usr/local/Ascend/cann-9.1.0-beta.1
    ninja -C /tmp/gst-ascend-dvpp-build-ascend
    meson test -C /tmp/gst-ascend-dvpp-build-ascend --print-errorlogs

For runtime validation, check the exact device nodes without relying on shell
globbing, then run the smallest finite pipeline before any stress test:

    find /dev -maxdepth 1 \( -name 'davinci*' -o -name 'devmm_svm' \) -ls
    npu-smi info -l

## Validation and Acceptance

The mock acceptance gate is a successful build, all tests passing, successful
registration of seven factories, and a finite `videotestsrc !
ascendvideoconvert ! fakesink` pipeline reaching EOS. The hardware acceptance
gate additionally requires a successful CANN build, visible device nodes, a
finite VPC pipeline reaching EOS, and JPEG encode/decode completion without
timeouts. Codec acceptance for the currently claimed matrix requires
complete-access-unit H.264 and H.265 streams, successful output, EOS drain,
channel teardown, and evidence that the report worker processed callbacks
rather than a timeout. Broader profiles and rate-control combinations remain
outside the current acceptance matrix.

The test suite must distinguish these levels in its output or documentation;
passing mock tests cannot satisfy hardware acceptance.

## Idempotence and Recovery

Build directories under `/tmp` are disposable and can be recreated. Do not
reset or discard the existing dirty worktree. Before each commit, inspect
`git diff --check`, stage only the intended files, run the relevant build/test
commands, and use the repository's commit-message writer to produce a message
with a `CHANGES:` section. A failed hardware run must remain documented as
unverified rather than being retried indefinitely.

## Artifacts and Notes

The primary artifacts are the source changes, the two-language README updates,
`DESIGN.md`, `docs/hardware-matrix.md`, and this plan. Runtime measurements are
valid only when their command, CANN version, target product, stream settings,
and sample duration are recorded beside the result.

## Interfaces and Dependencies

The build uses GStreamer 1.20, GLib, Meson, Ninja, and optionally CANN ACL DVPP
and AscendCL. Hardware builds require `acl/acl_rt.h`,
`acl/ops/acl_dvpp.h`, `libacl_dvpp.so`, and `libascendcl.so` under the configured
Ascend prefix. The VPC implementation must preserve the public properties
`device-id`, `inflight-depth`, and `stream-count`. The allocator must preserve
`gst_ascend_buffer_get_device_data()`, which is the boundary used to reuse a
DVPP pointer without an intermediate host copy.
