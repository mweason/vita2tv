# vita2tv: UVC + UAC composite USB architecture

Investigation report. No code has been changed. Read this before Milestone 1.

**Verdict up front:** combining UVC video and UAC stereo audio in one USB
configuration is technically viable and fits inside every SceUdcd limit with
room to spare. The hard problem is not USB — it is that **neither repository
captures Vita game audio**. `psvita-usb-audio-midi` gives us the transport
half and none of the capture half. See [§6](#6-minimum-to-borrowand-what-must-be-built).

---

## 1. Current vita2tv USB/video architecture

One kernel module, `udcd_uvc.skprx`, built from:

| File | Lines | Role |
|---|---|---|
| `src/main.c` | 1060 | Everything: driver callbacks, IFTU CSC, frame pump, module lifecycle |
| `include/usb_descriptors.h` | 455 | All UVC + SceUdcd descriptor tables (static initialisers) |
| `include/uvc.h` | 706 | UVC spec constants and structs, copied from Linux `uvcvideo` |
| `debug/*` | — | Optional framebuffer console, `DEBUG=1` only |

### 1.1 Where SceUdcd is initialised

`ksceUdcdRegister(&uvc_udcd_driver)` in `module_start` (`src/main.c:995`).
The driver struct itself is `src/main.c:508`. Actual bus takeover happens later,
on the plugin's own thread, in `uvc_start()` (`src/main.c:852`).

### 1.2 Plugin startup

`module_start` (`src/main.c:960`):

1. `taiGetModuleInfoForKernel(KERNEL_PID, "SceUdcd", …)`, then
   `taiHookFunctionOffsetForKernel(…, 0x01E1128C - 0x01E10000, 1, SceUdcd_sub_01E1128C_hook_func)`.
   SceUdcd ignores `SceUdcdConfigDescriptor.extra`, so the 8-byte Interface
   Association Descriptor is memmove'd into the serialised config descriptor by
   hand and `wTotalLength` bumped by 8 (`src/main.c:936`). **The hook's return
   value is never checked.**
2. Create `uvc_thread` (priority `0x3C`, stack `0x1000`) and `uvc_event_flag`.
3. `ksceUdcdRegister`.
4. Start the thread.

`uvc_thread` (`src/main.c:762`) then calls `uvc_start()`:

- `ksceDisplayWaitSetFrameBufCB()`, then a 15-second `ksceKernelDelayThreadCB`
  (non-DEBUG builds) to let LiveArea settle.
- `ksceUdcdDeactivate()`, then `ksceUdcdStop()` on `USB_MTP_Driver`,
  `USBPSPCommunicationDriver`, `USBSerDriver`, `USBDeviceControllerDriver`.
- `ksceUdcdStart("USBDeviceControllerDriver")` → `ksceUdcdStart("VITAUVC00")`
  → `ksceUdcdActivate(0x1337)`.
- `uvc_frame_req_init()` creates `uvc_frame_req_evflag`.
- Copies `uvc_probe_control_setting_default` into `uvc_probe_control_setting`.

It registers a vblank callback (`display_vblank_cb_func`, `src/main.c:736`)
that counts vblanks and signals `uvc_event_flag` once the host-negotiated
`dwFrameInterval` has elapsed. That callback is the frame clock.

The loop waits on `uvc_event_flag` with a 1-second timeout; on signal it calls
`send_frame()`, on timeout it calls `uvc_frame_term()` to release the frame
buffer while idle.

### 1.3 How video streaming endpoints are configured

`send_frame()` (`src/main.c:679`):

1. `ksceDisplayGetPrimaryHead()` + `ksceDisplayGetProcFrameBufInternal(-1, head, 0|1, &fb_info)`
   yields the **physical address** of the live framebuffer.
2. `wWidth`/`wHeight` are read out of
   `video_streaming_descriptors.frames_uncompressed_nv12[bFrameIndex - 1]` —
   i.e. straight from the descriptor table the host selected.
3. If the frame index changed, `uvc_frame_term()` + `uvc_frame_init()`
   reallocate a PHYCONT, 4 KiB-aligned memblock (type `0x10208006`).
4. `frame_convert_to_nv12()` (`src/main.c:578`) fills `SceIftuConvParams`,
   `SceIftuPlaneState_updated` (source) and `SceIftuFrameBuf` (dest) and calls
   `ksceIftuCsc()` — hardware colour-space conversion, `dst.paddr1 = paddr0 + w*h`
   for the NV12 chroma plane, scaling by 16.16 inverse factors.
5. `uvc_frame_transfer()` (`src/main.c:531`) writes the 12-byte UVC payload
   header (`bHeaderLength`, then `EOH | FID | EOF`) and calls
   `uvc_frame_req_submit_phycont()` (`src/main.c:200`): **one**
   `ksceUdcdReqSend` with `SCE_UDCD_DEVICE_REQUEST_ATTR_PHYCONT` covering header
   plus payload, then **blocks `uvc_thread`** on `uvc_frame_req_evflag` until the
   completion callback fires.

The whole frame is one bulk transfer DMA'd directly out of the CSC destination
buffer. No CPU ever touches a pixel. `struct uvc_frame` (`src/main.c:78`) pads
by `16 − 12 = 4` bytes so `data[]` lands 16-byte aligned for IFTU while the USB
controller starts reading at `header`.

> **This blocking wait is the single most important constraint on the audio
> design.** Audio must never run on `uvc_thread` and must never contend with it
> for a lock.

### 1.4 Control-request handling

`uvc_udcd_process_request` (`src/main.c:376`) dispatches on `bmRequestType`.
Class-to-interface requests split on `wIndex & 0xFF`:

- `CONTROL_INTERFACE` (0) → sub-dispatch on `wIndex >> 8` (unit/terminal ID);
  all three handlers are log-only stubs.
- `STREAM_INTERFACE` (1) → `uvc_handle_video_streaming_req` (`src/main.c:288`).

Probe/Commit `GET_*` are answered inline by `usb_ep0_req_send`. `SET_CUR` is
deferred: `usb_ep0_enqueue_recv_for_req` → `usb_ep0_req_recv_on_complete`
(`src/main.c:264`) → `uvc_handle_video_streaming_req_recv` (`src/main.c:228`),
which stores `bFormatIndex`, `bFrameIndex`, `dwFrameInterval` and, on
`COMMIT/SET_CUR`, sets `stream = 1` and kicks the event flag.

**Resolution and frame rate are entirely host-selected.** The Vita advertises
five frame descriptors and stores whatever the host commits. Nothing in the
plugin chooses a mode. This must stay true.

Stop paths, both routed to `uvc_handle_video_abort()` (`src/main.c:339`):

- `SET_INTERFACE` alt 0 on the stream interface — macOS.
- `CLEAR_FEATURE(ENDPOINT_HALT)` on endpoint 1 — Windows.

Both clear `stream`, then `ksceUdcdClearFIFO(&endpoints[1])` and
`ksceUdcdReqCancelAll(&endpoints[1])`.

### 1.5 Cleanup and shutdown

`module_stop` (`src/main.c:1017`) clears `uvc_thread_run`, signals the event
flag, and joins the thread. The thread's own exit path runs `uvc_stop()`
(`src/main.c:988`), which deactivates, stops `VITAUVC00` and the controller,
restarts the controller and `USB_MTP_Driver`, and calls
`ksceUdcdActivate(0x4E4)` — the stock MTP product ID. Then `module_stop`
deletes the event flag and thread, runs `uvc_frame_req_fini()`, deactivates,
stops, unregisters, and releases the taiHEN hook.

**`uvc_stop()` restores unconditionally.** It restarts MTP and activates
`0x4E4` regardless of what was running before the plugin took over. See §6 for
a strictly better pattern available from the reference project.

### 1.6 Interfaces and endpoints in use today

| # | Interface | Class / subclass | Alt settings | Endpoints |
|---|---|---|---|---|
| 0 | VideoControl | `0x0E` / `0x01` | 0 only | none |
| 1 | VideoStreaming | `0x0E` / `0x02` | 0 only | `0x81` |

`0x81` is **BULK**, `wMaxPacketSize` 512 (high speed) / 64 (full speed),
`bInterval` 0.

```c
SceUdcdEndpoint endpoints[2] = {{USB_ENDPOINT_OUT, 0, 0, 0},   /* EP0     */
                                {USB_ENDPOINT_IN,  1, 0, 0}};  /* video   */
SceUdcdInterface interface    = {.expectNumber = -1, .interfaceNumber = 0,
                                 .numInterfaces = 2};
/* uvc_udcd_driver.numEndpoints = 2 */
```

Device descriptor: `bcdUSB 0x0200` on both speeds, `bDeviceClass 0xEF`
(Miscellaneous) / `0x02` (Common) / `0x01` (IAD), `idVendor 0`, `idProduct 0`
(the PID arrives via `ksceUdcdActivate(0x1337)`), `iProduct 2` = "PSVita",
`iSerialNumber 3` = "UDCD UVC".

Config descriptor `wTotalLength` is a hand-summed expression at
`include/usb_descriptors.h:319` that deliberately **excludes** the IAD (the
line is commented out) because the taiHEN hook adds those 8 bytes at runtime.

---

## 2. Relevant parts of psvita-usb-audio-midi

`intermynd-instruments/psvita-usb-audio-midi`, MIT, © 2026 Intermynd
Instruments. ~5,600 lines, CMake, split kernel `.skprx` + user `.suprx`.

| File | Lines | Relevance |
|---|---|---|
| `src/kernel/usb_driver.c` | 1258 | **High** — the SceUdcd driver: descriptors, endpoints, iso submit/complete |
| `src/kernel/kernel.c` | 982 | **Medium** — lifecycle, worker thread, syscalls, lease, takeover orchestration |
| `src/common/audio_core.c/.h` | 359/132 | **High** — ring buffer, packet clock, request state machine |
| `src/common/usb_descriptors.c/.h` | 145/73 | **High** — UAC1 class descriptor byte arrays + layout validator |
| `src/common/takeover.c/.h` | 149/42 | **High** — ordered takeover and restore |
| `src/common/stock_state.c/.h` | 68/11 | **High** — classify SceUdcd driver/device state |
| `src/common/owner_guard.c/.h` | 28/18 | Low — per-process lease, vita2tv has no owning app |
| `src/client/*` | ~150 | Low — user bridge; useful as a *pattern* for the audio tap |
| `src/common/midi_*.c` | ~180 | **Not needed** |
| `src/common/audio_diagnostics.c`, `src/audio-midi-scope/` | ~1300 | **Not needed** |

### 2.1 Critical finding: it does not capture Vita audio

`grep -rn "taiHook\|SceAudio\|ksceAudio"` over `src/` and `include/` returns
**nothing**. There is no hook of any kind in the project.

"Master L/R" in its documentation means the **owning application's** master mix.
The app calls `psvitaUsbAudioWriteMulti(pcm, frames, 48000, 10)` and the kernel
plugin only moves those bytes to USB. From `docs/AUDIO.md`:

> An application must resample or convert its audio before calling the plugin.

The project was built for one app (DS-8 Drumstream) that already owns its audio.
For vita2tv's goal — game audio, with no cooperation from the game — this
project supplies the **USB/UAC transport half and none of the capture half**.

### 2.2 How its USB Audio descriptors are constructed

`src/common/usb_descriptors.c` holds raw `uint8_t` arrays attached through
`SceUdcdInterfaceDescriptor.extra` and `SceUdcdEndpointDescriptor.extra`:

- **AudioControl extra, 53 B** — AC header (`bcdADC 0x0100`, `wTotalLength 53`,
  `bInCollection 3`, `baInterfaceNr {1,2,3}`), Input Terminal ID 1, Output
  Terminal ID 2, Input Terminal ID 3, Output Terminal ID 4.
- **AudioStreaming extra, 18 B** — `AS_GENERAL` + Type I `FORMAT_TYPE` with one
  discrete frequency `0x00BB80` (48000).
- **Endpoint extra, 9 B** — and here is the non-obvious trick:

```c
const uint8_t psvita_usb_audio_in_extra[9] = {
        0, 0,                        /* bRefresh, bSynchAddress            */
        7, 0x25, 0x01, 0, 0, 0, 0    /* CS_ENDPOINT, EP_GENERAL            */
};
```

SceUdcd only models the common **7-byte** endpoint descriptor. The audio class
requires the **9-byte** form. So the two extra bytes are prepended to `extra`
and the descriptor's own `bLength` field is hand-set to 9. **This is the single
most valuable thing to borrow** — it is what makes UAC possible on SceUdcd at
all.

`psvita_usb_audio_midi_validate_descriptor_layout()` walks every array with
`descriptor_chain_valid()` (each `bLength` must be ≥ 2 and land exactly on the
end) and cross-checks every derived constant. `midi_usb_register()` refuses to
register the driver if it fails.

### 2.3 Its interfaces and endpoints

| # | Interface | Class / subclass | Alt | Endpoint |
|---|---|---|---|---|
| 0 | AudioControl | `0x01` / `0x01` | 0 | — |
| 1 | AudioStreaming, Vita→host | `0x01` / `0x02` | 0, 1 | `0x83` iso IN, async (`bmAttributes 0x05`), 960 B, `bInterval` 4 HS / 1 FS |
| 2 | AudioStreaming, host→Vita | `0x01` / `0x02` | 0, 1 | `0x04` iso OUT, adaptive (`0x09`), 196 B |
| 3 | MIDIStreaming | `0x01` / `0x03` | 0 | `0x01` bulk OUT, `0x82` bulk IN |

`PSVITA_USB_UDCD_ENDPOINT_COUNT = 5` (EP0 + 4). Config `wTotalLength` 249,
`bNumInterfaces` 4. Device descriptor uses `bDeviceClass USB_CLASS_PER_INTERFACE`
(0), VID `0x054C`. **No Interface Association Descriptor** — the AC header's
`baInterfaceNr` collection binds the function.

### 2.4 How audio buffers reach USB

From `audio_core.h` and `usb_driver.c`:

- **Ring:** 8192 frames × 10 channels, drop-oldest (`PsvitaUsbAudioRing`).
  The endpoint starts consuming after 6144 frames are buffered
  (`PSVITA_USB_AUDIO_PRIME_FRAMES`), leaving 2048 frames of write headroom.
- **Packet clock:** `psvita_usb_audio_packet_frames()` ignores occupancy and
  always returns 48 frames per service interval. The stream is nominally
  rate-locked; drift is absorbed by the ring plus conceal (hold last frame) and
  rebuffer (after 384 consecutive missing frames).
- **Batching:** `PSVITA_USB_AUDIO_REQUEST_INTERVALS = 8` — one `ksceUdcdReqSend`
  carries 8 service intervals, an 8 ms completion cadence.
  `psvita_usb_audio_request_intervals()` shrinks the batch to whole packets when
  a primed ring holds less than one full batch.
- **One request in flight, always:**

```c
/* Vita SceUdcd stalls the isochronous endpoint if requests are queued ahead. */
#define PSVITA_USB_AUDIO_MAX_IN_FLIGHT_REQUESTS 1u
```

- **Buffer:** one 8 KiB `ksceKernelAllocMemBlock` of
  `SCE_KERNEL_MEMBLOCK_TYPE_KERNEL_ROOT_NC_RW` (non-cacheable), PHYCONT and
  8192-aligned, submitted with `SCE_UDCD_DEVICE_REQUEST_ATTR_PHYCONT`.
- **Request lifecycle:** a five-state machine
  (`FREE → PREPARING → PENDING → CANCELING|COMPLETING → FREE`) so the completion
  callback and the worker thread cannot race a slot. A failed `ksceUdcdReqSend`
  calls `psvita_usb_audio_read_rollback()` to restore the ring read pointer and
  packet-clock state, so a retry sends the same audio rather than inventing a
  shortage.
- **Stall recovery:** a request pending > 250 ms (`AUDIO_REQUEST_STALL_US`) is
  cancelled with `ksceUdcdReqCancelAll` and the FIFO cleared on the next submit.

### 2.5 Synchronisation and timing

SET_INTERFACE is handled in **two phases**, and the details encode real hardware
findings:

- `process_request` decodes SET_INTERFACE and records the *requested* alternate
  plus a timestamp. It does not act.
- SceUdcd's `changeSetting` callback is what normally applies it.
- Because some firmware never calls `changeSetting`,
  `midi_usb_audio_submit_next()` applies alternate 1 itself after 20 ms
  (`AUDIO_ALTERNATE_FALLBACK_US`) and counts it in `fallback_starts`.
- After the switch it waits a further 5 ms (`AUDIO_ALTERNATE_SETTLE_US`) before
  the first submit: *"Even changeSetting may run while the controller is
  finishing the EP switch."*
- Alternate **0** is applied immediately and synchronously — *"Stop immediately;
  waiting for a second callback can leak a request."*

The worker thread (`kernel.c:345`, priority `0x3C`, stack `0x3000`) waits on an
event flag with a timeout. Every completion callback sets `WORK_AUDIO` via
`midi_kernel_usb_audio_ready()`, so submission is completion-driven with a
timeout backstop.

### 2.6 The user-mode bridge

`src/kernel/exports.yml` declares library `PsvitaUsbAudioMidiForUser` with
`syscall: true` and 16 fixed NIDs. `src/client/client.c` is a thin `.suprx` that
forwards each `psvitaUsb*` call to the matching `kscePsvitaUsb*` syscall and
tracks `lease_owned` so its own `module_stop` releases the lease. `loader.c` plus
a weak stub library let an app start when the plugin is absent.

Syscalls copy user memory into bounded kernel buffers under a dedicated
`audio_write_mutex`, separate from the general `state_mutex`.

### 2.7 Lease and ownership

`kscePsvitaUsbAudioMidiAcquire(flags)` records `owner_pid`. Notably,
`driver_conflicts()` (`kernel.c:118`) refuses to take over the bus when either
of these is registered:

```c
return ksceUdcdGetDrvStateInternal("VITAUVC00", bus) >= 0 ||
       ksceUdcdGetDrvStateInternal("VITASTICK", bus) >= 0;
```

`VITAUVC00` is **this project's driver name**. The reference already treats
vita2tv as mutually exclusive — which is exactly the "two competing USB drivers"
failure this work exists to avoid.

Release paths: explicit `Release()`, a `SceProcEventHandler` on process
exit/stop, and a periodic watchdog that calls `ksceKernelGetProcessInfo` on the
owner pid and force-releases if the process is gone.

### 2.8 How it restores the original USB state

`snapshot_stock_state()` reads `ksceUdcdGetDrvStateInternal` for
`USBDeviceControllerDriver`, `USB_MTP_Driver`, `USBPSPCommunicationDriver`,
`USBSerDriver`, plus `ksceUdcdGetDeviceStateInternal`.
`psvita_usb_audio_midi_classify_stock_state()` turns those into
`{mtp_started, psp_comm_started, serial_started, usb_active}`.

Takeover is strictly ordered with rollback on any failure: deactivate (only if
active) → `ksceUdcdStopCurrentInternal` → start controller → start own driver →
activate. Restore replays the snapshot: deactivate, stop own driver, start
controller, restart **only** the drivers observed started beforehand, and
re-`ksceUdcdActivate(STOCK_MTP_PID)` only if USB was active with MTP.

This is materially better than vita2tv's unconditional `uvc_stop()` and is worth
adopting on its own merits.

---

## 3. Source files and functions involved

**vita2tv**

| Concern | Location |
|---|---|
| SceUdcd registration | `src/main.c:995` (`module_start`), driver struct `src/main.c:508` |
| Bus takeover / release | `uvc_start()` `src/main.c:852`, `uvc_stop()` `src/main.c:988` |
| Descriptors | `include/usb_descriptors.h` — `endpoints[2]`:214, `interface`:220, `endpdesc_hi`:260, `interdesc_hi`:279, `settings_hi`:315, `confdesc_hi`:319 |
| IAD injection hook | `SceUdcd_sub_01E1128C_hook_func` `src/main.c:936` |
| Control requests | `uvc_udcd_process_request` `src/main.c:376`, `usb_ep0_req_recv_on_complete` `src/main.c:264` |
| Frame pump | `uvc_thread` `src/main.c:762`, `display_vblank_cb_func` `src/main.c:736`, `send_frame` `src/main.c:679` |
| CSC + DMA | `frame_convert_to_nv12` `src/main.c:578`, `uvc_frame_req_submit_phycont` `src/main.c:200` |
| Buffer alloc | `uvc_frame_init` `src/main.c:805`, `uvc_frame_term` `src/main.c:844` |
| Teardown | `uvc_handle_video_abort` `src/main.c:339`, `module_stop` `src/main.c:1017` |

**psvita-usb-audio-midi**

| Concern | Location |
|---|---|
| Descriptor byte arrays | `src/common/usb_descriptors.c:12–79` |
| Layout validator | `psvita_usb_audio_midi_validate_descriptor_layout` `usb_descriptors.c:93` |
| SceUdcd tables | `src/kernel/usb_driver.c:32–200` |
| Driver struct + register | `usb_driver.c:787`, `midi_usb_register` `usb_driver.c:797` |
| Iso submit | `midi_usb_audio_submit_next` `usb_driver.c:~930` |
| Iso completion | `audio_complete` `usb_driver.c:~460` |
| Alternate handling | `apply_audio_streaming_alternate` / `request_audio_streaming_alternate` `usb_driver.c:~600`, `change_setting` `usb_driver.c:~703` |
| Ring + packet clock | `src/common/audio_core.c` (`ring_write`, `ring_read_packet`, `read_checkpoint`, `read_rollback`, `packet_frames`, `request_intervals`) |
| Request state machine | `audio_core.c` (`request_try_prepare`, `publish`, `begin_completion`, `finish_completion`, `try_cancel`, `is_stalled`) |
| Worker thread | `worker_main` `src/kernel/kernel.c:345`, `process_audio` `kernel.c:314` |
| Takeover / restore | `src/common/takeover.c`, `src/common/stock_state.c`, ops table `kernel.c:98` |
| Conflict check | `driver_conflicts` `kernel.c:118` |

---

## 4. USB interfaces and endpoints used by each

**vita2tv today:** interfaces 0–1, endpoints EP0 + `0x81` bulk IN.

**psvita-usb-audio-midi:** interfaces 0–3, endpoints EP0 + `0x83` iso IN +
`0x04` iso OUT + `0x01`/`0x82` bulk MIDI.

**Proposed combined device:**

| # | Interface | Class | Alt | Endpoint | Owner |
|---|---|---|---|---|---|
| 0 | VideoControl | `0x0E`/`0x01` | 0 | — | video (unchanged) |
| 1 | VideoStreaming | `0x0E`/`0x02` | 0 | `0x81` bulk IN, 512 B | video (unchanged) |
| 2 | AudioControl | `0x01`/`0x01` | 0 | — | audio (new) |
| 3 | AudioStreaming | `0x01`/`0x02` | 0, 1 | `0x82` iso IN, async `0x05`, 196 B, `bInterval` 4 HS / 1 FS | audio (new) |

Keeping video at interfaces 0/1 means the existing IAD
(`bFirstInterface 0, bInterfaceCount 2`) stays byte-identical and OBS/QuickTime
see an unchanged video function.

---

## 5. Is a combined UVC + UAC device viable?

**Yes**, on the USB side, with real headroom. Against the VitaSDK limits in
`psp2kern/udcd.h`:

| Resource | Limit | Combined need | Headroom |
|---|---|---|---|
| Interfaces | `SCE_UDCD_MAX_INTERFACES` 8 | 4 | 4 |
| Endpoints (incl. EP0) | `SCE_UDCD_MAX_ENDPOINTS` 9 | 3 | 6 |
| Alternate settings | `SCE_UDCD_MAX_ALTERNATE` 2 | 2 (UAC stream alt 0/1) | **0** |
| IN endpoints in hardware | 2 proven by the reference | 2 | 0 known |

Bandwidth at high speed is a non-issue. 48 kHz × 2 ch × 16-bit = **192 B/ms =
0.19 MB/s** of reserved isochronous bandwidth. Worst-case video, 1280×720 NV12
at 30 fps, is roughly **41 MB/s** of bulk. Isochronous is reserved first and
bulk consumes the remainder; audio takes under 0.4% of the theoretical 60 MB/s
bus. Audio will not meaningfully starve video.

Class coexistence is ordinary: every USB capture card on the market is a
UVC + UAC composite, and macOS, Windows, and Linux all bind the two functions
independently.

Two things stop this from being a clean "yes":

1. **No audio source exists** in either repository (§2.1, §6).
2. **Alternate settings are at the hard limit** with zero spare, and isochronous
   traffic concurrent with a saturating bulk stream is untested on this
   controller (§8).

---

## 6. Minimum to borrow — and what must be built

### 6.1 Borrow and adapt (MIT, roughly 500 lines)

| From | Take | Why this and not more |
|---|---|---|
| `src/common/usb_descriptors.c/.h` | The UAC1 AC/AS/endpoint `extra` byte-array pattern — above all the 2-byte `bRefresh`/`bSynchAddress` prefix with `bLength = 9` — and `descriptor_chain_valid()` | This is the non-obvious part of making UAC work on SceUdcd |
| `src/common/audio_core.c/.h` | `PsvitaUsbAudioRing` (init/write/read_packet/available), `PsvitaUsbAudioPacketClock`, `PsvitaUsbAudioRequestState` and its helpers, `read_checkpoint`/`read_rollback` | Drop-oldest ring plus rollback-safe submit is the part with hardware miles on it |
| `src/kernel/usb_driver.c` | The submit/complete pattern: one request in flight, PHYCONT non-cacheable memblock, slot matching in `audio_complete`, stall recovery, two-phase SET_INTERFACE with the 20 ms fallback and 5 ms settle | Those timings and the 1-in-flight rule are recorded hardware findings, not style |
| `src/common/takeover.c/.h`, `stock_state.c/.h` | Both files, close to verbatim | Strictly better USB restore than the current `uvc_stop()` — worth adopting even independent of audio |

Cut to stereo throughout: `STREAM_CHANNELS` 10 → 2, ring 8192 × 2, packet
192 B/interval, drop the per-channel peak instrumentation.

### 6.2 Explicitly skip

All MIDI (`midi_core.c`, `midi_timing.c`), `audio_diagnostics.c`,
`src/audio-midi-scope/`, host-to-Vita input, the ten-channel path, and the
lease/`owner_guard`/`exports.yml`/`client/` layer — vita2tv has no owning app,
the plugin is always resident. Keep their CMake build out; vita2tv's Makefile is
fine.

### 6.3 Build new: the audio tap

This does not exist in either repository and is the bulk of the new work.

- VitaSDK exposes **no** kernel-side audio driver library.
  `vita-headers/db/360/SceAudio.yml` declares exactly one library, `SceAudio`,
  marked `kernel: false`. There is no `SceAudioForDriver` to import.
- So the tap must reach the user-mode `SceAudio` module. Two options:

  **(a) A `*ALL` user plugin** that hooks `sceAudioOutOutput` (NID `0x02DB3F5F`)
  with `taiHookFunctionImport`/`taiHookFunctionExport`, copies the PCM, and hands
  it to the kernel plugin through a syscall. Mirrors the reference's
  client/kernel split.

  **(b) Kernel-side `taiHookFunctionExportForKernel(pid, "SceAudio", …)`**
  installed per-process from a `SceProcEventHandler`, keeping everything in one
  `.skprx`.

- **Recommendation: (a).** It is testable in isolation, cannot panic the kernel,
  and matches a proven split. Its risk is that it loads into every process —
  the same class of change behind the crashes already observed with another
  video/audio implementation. Mitigate by keeping the hook trivial: one bounded
  `memcpy` into a ring, never block, never allocate, never log.

- **Format work the reference punts to its app.** `sceAudioOutOutput` delivers
  whatever the game opened its port with via `sceAudioOutOpenPort` — grain size,
  mono or stereo, and a sample rate that is not guaranteed to be 48 kHz (voice
  ports in particular). We need mono→stereo expansion, rate conversion, and
  mixing when a title holds several ports open at once. **The exact set of rates
  and port types the Vita permits must be confirmed on hardware in Milestone 3**
  — treat any assumption here as unverified.

---

## 7. Licensing and attribution

| Component | License | Obligation |
|---|---|---|
| `psvita-usb-audio-midi` | MIT, © 2026 Intermynd Instruments | Retain the copyright notice and full MIT permission text with any substantial derived portion |
| `xerpi/vita-udcd-uvc` (what vita2tv forks) | **None stated** | See below |
| `include/uvc.h` | Derived from Linux `include/uapi/linux/usb/video.h` (GPL-2.0 WITH Linux-syscall-note) and `drivers/media/usb/uvc/uvcvideo.h` (GPL-2.0) | Already in the tree; record it |
| VitaSDK vita-headers | MIT | Build-time only |

**Concrete steps for the MIT obligation:** add `LICENSES/psvita-usb-audio-midi-MIT.txt`
with the verbatim license, add a `THIRD_PARTY_NOTICES.md`, and head each adapted
file with:

```c
/* SPDX-License-Identifier: MIT
 * Adapted from psvita-usb-audio-midi, (c) 2026 Intermynd Instruments.
 * https://github.com/intermynd-instruments/psvita-usb-audio-midi
 */
```

Their README also *asks* (does not require) for a credit link back to the
project and to Intermynd Instruments. Cheap to honour; do it.

**The upstream licensing gap.** Neither this repository nor `xerpi/vita-udcd-uvc`
contains a LICENSE file. The reference project's own `THIRD_PARTY_NOTICES.md`
records the same observation:

> The project was informed by the public documentation and architecture of
> xerpi/vita-udcd-uvc and xerpi/vitastick. Those repositories do not currently
> state a project-wide open-source license.

vita2tv is therefore an unlicensed fork of unlicensed code. That is a
pre-existing condition, not something this work creates, but it means **vita2tv
cannot be released under MIT** — xerpi's code cannot be relicensed by a
downstream fork. Practical options: keep the repository license-silent and ship
accurate third-party notices, or ask xerpi to state a license. Worth resolving
before any public release; it does not block development.

---

## 8. Major technical risks

### High

**H1 · No audio source exists.** Everything in §6.3 is new work with no
reference implementation. Highest schedule risk, lowest USB risk. Do not let
the ease of the descriptor work create a false sense of progress.

**H2 · `wTotalLength` must be exactly right.** The taiHEN hook hand-injects the
IAD and bumps `wTotalLength` by 8. The base value is a hand-summed expression at
`include/usb_descriptors.h:319`. Adding UAC means adding, by hand: 2 interface
descriptors (18 B) + 1 endpoint descriptor (9 B, not 7) + 53 + 18 + 9 bytes of
class data. Get it wrong and the host reads a truncated or over-long config
descriptor and **the whole device fails to enumerate, video included.**
*Mitigation:* port `descriptor_chain_valid()` and add a
`validate_descriptor_layout()` that fails `module_start` rather than enumerating
a broken descriptor; diff `lsusb -v` byte-for-byte against the Milestone 1
baseline for the UVC portion.

**H3 · Alternate settings are at the hard limit.** `SCE_UDCD_MAX_ALTERNATE` is 2
and UAC streaming needs both. If SceUdcd mishandles alternates when an *earlier*
interface declares only one setting, there is no headroom to work around it.
*Fallback:* the reference's own 20 ms `changeSetting` fallback path already
exists for precisely this class of firmware bug — port it from the start rather
than adding it after a failure.

**H4 · The video thread blocks on every frame.**
`uvc_frame_req_submit_phycont` waits on `uvc_frame_req_evflag` until the bulk
transfer completes. Audio must have its own thread and its own event flag, and
the audio submit path must never take a lock the video path holds. Design the
two lanes to share nothing but the `SceUdcdDriver` registration.

### Medium

**M1 · Isochronous alongside saturating bulk.** The reference's
`MAX_IN_FLIGHT_REQUESTS = 1` finding — *"SceUdcd stalls the isochronous endpoint
if requests are queued ahead"* — was measured with **no** concurrent bulk
traffic. A 41 MB/s video stream on the same controller is untested. Watch for
late or short iso completions and for spikes in video frame-transfer latency.
Port the reference's completion-gap and rearm-delay counters; they already
measure exactly this.

**M2 · Shared controller state in per-endpoint teardown.**
`uvc_handle_video_abort()` clears `endpoints[1]`; audio stop clears
`endpoints[2]`. Verify SceUdcd's `ClearFIFO`/`ReqCancelAll` do not reset shared
controller state. **Never** call `ksceUdcdDeactivate` from an audio stop path.

**M3 · Control-request dispatch has no branch for interfaces 2/3.** Both
`uvc_udcd_process_request` and `usb_ep0_req_recv_on_complete` switch on
`wIndex & 0xFF` with no default. Class requests aimed at the audio interfaces
will silently fall through until those branches are added.

**M4 · IAD or no IAD for the audio function.** The device declares
`0xEF/0x02/0x01`, meaning IADs are in use. UAC1 normally binds via the AC
header's `baInterfaceNr` collection with no IAD — and the reference works that
way — but its device class is `PER_INTERFACE (0)`. Windows *may* expect every
function to carry an IAD when the device advertises the IAD protocol.
*Plan:* start without a second IAD (minimal change). Only if a host fails to
bind audio, add one — which means extending the hook to a second memmove at a
computed offset, a materially riskier edit.

**M5 · MTP/VitaShell restore is unconditional.** `uvc_stop()` restarts
`USB_MTP_Driver` and activates `0x4E4` regardless of prior state. Audio does not
make this worse, but Milestone 6 should replace it with the reference's
snapshot/restore.

**M6 · The taiHEN offset hook is firmware-fragile and unguarded.**
`0x01E1128C - 0x01E10000` is a hardcoded offset into SceUdcd and
`taiHookFunctionOffsetForKernel`'s return value is never checked in
`module_start`. Pre-existing, but descriptor work amplifies the consequences.
Add a return check and a fail-safe path.

### Lower

**L1 · Memory.** 8192 frames × 2 ch × 2 B = 32 KiB ring, plus an 8 KiB PHYCONT
DMA block. Against a video frame buffer of up to ~1.3 MB, negligible.

**L2 · CPU.** One `memcpy` per audio callback and one per 8 ms USB batch.
Negligible beside the IFTU path, which is already DMA and touches no pixels.

**L3 · A/V sync.** The UVC payload header here is 12 bytes with only
`EOH`/`FID`/`EOF` — no `SCR` bit, no presentation timestamp. Audio and video are
independently clocked and hosts treat them as two unrelated devices sharing one
cable. OBS users will need a manual audio offset. Not a defect; document it.

**L4 · Game compatibility from the `*ALL` tap plugin.** This is the same
mechanism behind the instability already observed elsewhere. Keep the hook
trivial and ship a per-title opt-out.

---

## 9. Proposed file and module structure

`src/main.c` stays the video module. Video code does not move.

```
vita2tv/
├── include/
│   ├── usb_descriptors.h   extended: + UAC interfaces/endpoints. UVC bytes unchanged.
│   ├── uvc.h               unchanged
│   └── uac.h               NEW  UAC1 constants + AC/AS/EP extra byte arrays
├── src/
│   ├── main.c              video + module lifecycle; gains ~30 lines to start/stop audio
│   ├── audio_ring.c/.h     NEW  adapted ring + packet clock          (MIT, Intermynd)
│   ├── audio_usb.c/.h      NEW  iso submit/complete, SET_INTERFACE
│   │                            alternates, audio worker thread      (MIT, Intermynd)
│   ├── audio_tap.c/.h      NEW  kernel side of capture: receives PCM,
│   │                            mixdown + rate convert, writes ring
│   └── usb_state.c/.h      NEW  (M6) adapted takeover/stock_state    (MIT, Intermynd)
├── plugin_user/            NEW  `*ALL` user plugin
│   ├── main.c                   hooks sceAudioOutOutput, syscalls in
│   └── exports.yml
├── LICENSES/psvita-usb-audio-midi-MIT.txt
├── THIRD_PARTY_NOTICES.md
└── docs/USB_AUDIO_ARCHITECTURE.md   this document
```

The contract between modules is deliberately one-way, so video never waits on
audio:

```
sceAudioOutOutput hook  (user, per-process)
        │  syscall · bounded memcpy · never blocks
        ▼
   audio_tap ─────► audio_ring  (drop-oldest, 8192 frames x 2ch)
                         │
                         ▼  audio worker thread (own thread, own event flag)
                    audio_usb ──► ksceUdcdReqSend(EP 0x82, iso, PHYCONT, 1 in flight)

   uvc_thread ──► IFTU CSC ─────► ksceUdcdReqSend(EP 0x81, bulk, PHYCONT, blocking)
```

No shared mutex between the two lanes. The only shared object is the
`SceUdcdDriver` registration, touched only at start and stop.

---

## 10. Implementation plan

### Milestone 1 — baseline

**VitaSDK is not installed on this machine** (`arm-vita-eabi-gcc` is not on
`PATH`, `$VITASDK` is unset). Install it first.

Build all three variants via `gen_builds.sh` (`make`, `DISPLAY_OFF_OLED=1`,
`DISPLAY_OFF_LCD=1`). Flash and capture:

- Full `lsusb -v` on Linux and the macOS System Information USB dump.
- OBS at 960×544 60 fps and 1280×720 30 fps.
- QuickTime capture.
- A 30-minute in-game soak.

**These dumps are the regression oracle for every later milestone.**

### Milestone 2 — descriptors only, no audio traffic

Add UAC interfaces 2/3, iso IN `0x82`, and the AC/AS/EP extra arrays. Update
`numEndpoints` to 3, `interface = {-1, 0, 4}`, extend `settings_hi`/`settings_full`
to 4 entries, `bNumInterfaces = 4`, and recompute `wTotalLength`. Add
`validate_descriptor_layout()` called from `module_start`, failing the module
load rather than enumerating a broken descriptor.

No audio thread. No submissions.

Verify: device still enumerates; the UVC portion of `lsusb -v` is
**byte-identical** to Milestone 1; OBS and QuickTime unchanged; the host lists a
Vita audio input that produces silence. Ship this as its own tested step.

### Milestone 3 — capture

Build the `*ALL` user plugin, the syscall, `audio_tap`, and `audio_ring`. No USB
involvement. Verify with a logged counter that PCM arrives at 48 kHz stereo at
correct level with no dropouts, across mono ports, several simultaneous ports,
and any non-48 kHz port types found on hardware. Test game stability with the
hook installed and audio going nowhere.

### Milestone 4 — transmit

Wire `audio_usb` to the ring: iso submit, `audio_complete`, two-phase
SET_INTERFACE with the 20 ms fallback and 5 ms settle, one request in flight,
PHYCONT non-cacheable memblock, stall recovery.

Verify audio alone, video not streaming, on macOS and Windows: correct pitch
over 10 minutes (rate error shows up as drift), no dropouts, and 48 kHz stereo
visible in Audio MIDI Setup and the Windows Sound control panel.

### Milestone 5 — simultaneous

OBS video plus OBS audio; QuickTime video; game soak; sleep/wake; hot
unplug/replug mid-stream; audio started before video and video before audio.
Watch the ported counters for iso short and late completions and for video
frame-transfer latency.

### Milestone 6 — lifecycle

Adopt `stock_state` and `takeover` for snapshot/restore and make `uvc_stop()`
conditional. Add safe runtime audio start/stop independent of video. Handle
`module_stop` while audio is streaming.

### Milestone 7 — user interface

Only after Milestones 5 and 6 are stable.

### Decisions to settle before Milestone 2

| Decision | Recommendation |
|---|---|
| Audio IN endpoint address | `0x82` — next free. Confirm `driverEndpointNumber` must be sequential: `endpoints[2] = {USB_ENDPOINT_IN, 2, 0, 0}` |
| `wMaxPacketSize` | **196 B** (49 frames) rather than 192 B (48). The extra reserved bandwidth is immaterial and it leaves room for a future drift scheme without a descriptor change |
| `bInterval` | 4 at high speed (8 microframes = 1 ms), 1 at full speed — matches the reference |
| `bmAttributes` | `0x05` — isochronous, asynchronous — matches the reference |
| Second IAD for audio | Start without one; add only if a host fails to bind (see M4 in §8) |
