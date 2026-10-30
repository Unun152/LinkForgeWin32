# LinkForge Win32

LinkForge is a native Windows C++20 multi-mirror downloader. Each queue entry represents one file; its URLs are mirrors of that same file. Compatible mirrors can contribute separate HTTP byte ranges to one output file.

## Build

Requirements:

- Windows 10 or Windows 11
- Visual Studio 2022 with **Desktop development with C++**
- MSVC v143 and a Windows 10/11 SDK
- CMake 3.24 or newer (included with the Visual Studio C++ workload)

Open the extracted `LinkForgeWin32` folder in Visual Studio using **File → Open → Folder**. Visual Studio reads `CMakeLists.txt` and `CMakeSettings.json`. Select `x64-Debug` or `x64-Release`, then use **Build → Build All**.

Alternatively, from a Visual Studio Developer Command Prompt:

```bat
cmake -S . -B build -A x64
cmake --build build --config Release
```

## Add a download

1. Click **Add new file download**.
2. Paste one mirror URL per line. All URLs in this dialog must refer to the same file.
3. Choose the destination folder and optional output filename.
4. Choose **Parallel mirrors** (the default) or **Single mirror only** in the dialog. This choice is saved with the download and is not silently changed later. LinkForge checks the mirrors, compares their reported size and sampled contents, and validates byte-range support.

**Parallel mirrors** means LinkForge uses compatible mirrors concurrently and requests different byte ranges. If ranges are unavailable, the download fails with an explicit explanation; LinkForge does not silently switch to a single URL. Retryable HTTP/network errors (including HTTP 509/429/5xx) are retried according to the retry settings while the job remains in parallel mode. **Single mirror only** means one URL is used at a time, with the next mirror tried only if needed. This is an explicit choice made for that file.

The mirror details window shows total mirror count, active/removed mirror count, the actual chunk size selected for that file, total completed/active/partial chunks, and per-mirror current/average speed, bytes received, HTTP status, active connections, and completed/active/failed chunk counts. The main-window status line also shows the current global number of active HTTP request slots across downloads, including mirror probes, range transfers, and full-file requests. Per-mirror chunk counts describe assignments completed during the current application session; the total completed-chunk count is derived from the durable file state. Mirrors can be added or removed while a download is running.

The settings dialog uses explicit white owner-drawn labels so text remains readable on dark Windows themes. The main queue uses native list-view painting for ordinary text cells and custom drawing only for the progress cell to avoid hover redraw flicker.

## Pause, persistence, and recovery

The main-window **Pause** button suspends the selected download without changing its chosen transfer mode. Active byte-range requests stop at a safe read boundary, flush bytes already received, and save their partial offset. The current parallel worker pool is joined cleanly; on Resume a new pool is created using the same segment table, completed chunks, partial offsets, mirrors, and parallel-only mode. The partial file is checked and reused, never recreated just because the user paused. Completed ranges remain completed. The status changes from **Pausing** to **Paused** only after active range writes have drained and their final offsets have been recorded. The Resume button is disabled during this flush phase. On application shutdown, a separate internal shutdown-stop request preserves the resumable partial file and queue record: user-paused jobs reopen as a fully settled **Paused** state, while active jobs automatically resume. Explicitly cancelled jobs remain cancelled.

Two separate INI files are stored beside `LinkForge.exe`:

- **`LinkForge.ini`** stores network/retry/User-Agent settings and the last selected destination under `[Paths] LastSaveFolder`.
- **`LinkForgeDownloads.ini`** stores the queue, output paths, mirrors, resume information, and completed parallel byte ranges.

The queue file is checkpointed approximately every two seconds, and after user actions such as adding a download, pausing, retrying, or changing mirrors. Checkpoints are written to a temporary file, flushed, then atomically replace the previous queue file. Completed byte ranges are flushed to the partial output before they are marked complete in the queue state. After a sudden shutdown, any range that was written but not checkpointed is simply downloaded again; it is never trusted as completed based only on the size of the partial file. Parallel workers acquire a global connection slot only while making an actual request, so paused or idle worker threads do not reserve network capacity from other downloads.

For parallel downloads, LinkForge can resume completed byte ranges after rechecking the mirror size and saved prefix/middle/end sample hashes. If those checks no longer match, the old ranges are discarded and the download safely restarts rather than combining different file versions. Sample hashes reduce accidental mismatches but are not a cryptographic proof; a trusted SHA-256 checksum would provide stronger end-to-end verification.

A server that does not support byte-range requests cannot reliably resume a partially downloaded full-file response after the process has been closed. In that fallback mode, a reopened job must start the file again from byte zero. Pausing a fallback transfer flushes the data already received, and pause/resume during the same running session keeps its current connection. After an application restart, the fallback request must start over because the server offers no safe byte offset to request. A long pause can also cause an idle server connection to time out and be retried.

Both INI files are kept beside the executable, not in AppData. The executable folder must be writable. If the queue is empty, `LinkForgeDownloads.ini` is removed. **Remove entry** deletes the final output, temporary partial file(s), and the queue record; it reports a Windows file-deletion error rather than silently dropping the record if a file is locked.

## Settings

Click **Settings** in the main window. Every setting has a tooltip explaining its effect.

| Setting | Default | Meaning |
|---|---:|---|
| Maximum simultaneous connections across all downloads | 8 | Application-wide active HTTP request limit across the queue; maximum 1024. WinHTTP remains capped at 64 connections per individual server. |
| Maximum parallel connections to each mirror | 2 | Per-URL concurrent range-request limit |
| Gradual connection ramp-up | On | Starts each mirror with one range request and permits more after successful transfers |
| Ramp-up delay | 500 ms | Minimum wait before adding another concurrent request to a mirror |
| Retry temporary failures and throttling | On | Retries network failures and HTTP 408/425/429/5xx responses, including HTTP 509 |
| Retry delay | 5 seconds | Fallback wait when a server does not provide a `Retry-After` header |
| Maximum retries per mirror | 0 (unlimited) | Number of retries after the first attempt; zero means keep retrying until success or cancellation |
| Custom User-Agent header | On | Sends the text in the User-Agent field with each HTTP request |
| Chunk size (MB) | 0 (automatic) | 0 calculates `targetChunks = max(8, compatibleMirrors × 4)`, then `ceil(fileSize / targetChunks)`, clamped to 64 KiB–4 MiB; 1–64 uses a fixed chunk size in MiB |

The global connection setting is an application-wide limit and can be configured from 1 to 1024 active HTTP requests across all downloads. This is not a Windows-wide 64-connection limit. The WinHTTP session remains separately configured for up to 64 connections to any individual server; raising the LinkForge global limit does not change that per-server cap. Each mirror is also governed by the configured per-mirror limit (1–32). Large global values can create many worker threads and consume substantial CPU, memory, socket, and bandwidth resources, while remote hosts may throttle or reject excessive concurrency. The default remains 8; increase the setting gradually and only when your workload benefits from it.

The connection ramp-up is per mirror: LinkForge initially sends at most one active range request to a mirror. After successful range transfers, it can gradually increase that mirror's concurrency up to the configured limit. Other mirrors can each start with their own first request. When adaptive throttling is enabled, a 509/429 response lowers that mirror's own connection cap by one (not below one); three subsequent successful range transfers increase the cap by one until it reaches the configured maximum.

HTTP 509 is treated as temporary server-side throttling, not as proof that byte ranges are unsupported. LinkForge respects a server's `Retry-After` header when present; otherwise it uses the configured retry delay. Other mirrors continue while one mirror waits.

## Cancel and remove

**Cancel download** asks whether to keep or delete the data already downloaded. Keeping it preserves an incomplete `.partial` file for inspection; it is not automatically treated as a completed download. **Remove entry** is the destructive operation: it deletes the output, partial files, and the saved queue record.

## Technical notes and limitations

- Uses Win32, WinHTTP, C++20, and the Windows SDK; no Qt or embedded browser.
- HTTP and HTTPS URLs are supported.
- Parallel assembly requires compatible file sizes and matching samples from all contributing mirrors. This is a consistency check, not a cryptographic integrity guarantee.
- Some hosts require cookies, authorization headers, referers, or other per-mirror request headers. The current GUI exposes only a User-Agent override.
- Hosting providers can enforce bandwidth limits or anti-abuse policies. LinkForge reduces concurrency and retries temporary throttling responses; it does not bypass server limits.


## Chunk scheduling

Automatic chunk sizing uses the file size and count of compatible range-capable mirrors. It calculates `targetChunks = max(8, compatibleMirrors × 4)`, calculates `chunkBytes = ceil(fileSize / targetChunks)`, then clamps the chosen chunk size between 64 KiB and 4 MiB (the final chunk can be shorter). For example, 2 compatible mirrors target 8 chunks, while 5 compatible mirrors target 20 chunks. The Settings dialog can override this with a fixed 1–64 MiB chunk size; 0 means Automatic. Each download stores its selected chunk size in `LinkForgeDownloads.ini`, so changing the global setting does not change chunk boundaries of an existing/resumed download. The details window displays the chosen chunk size and total/per-mirror chunk counters. Completed chunks are assigned to exactly one mirror; an interrupted chunk retains its flushed byte offset and is retried from that offset.

## Speed and ETA calculation

The main queue's **Speed** value is a short-interval network-throughput reading and can react quickly to bursts. **ETA** uses the same stable average network throughput displayed as **Avg** in the file/mirror details window: total received bytes divided by elapsed time since the first response bytes arrived. The remaining file bytes are divided by that average to estimate the remaining time. This avoids a rolling-window warm-up that could make the ETA disappear periodically. Retransmitted bytes are included in the average, so repeated failed ranges can make the estimate optimistic; the average also naturally adjusts more slowly to sudden speed changes. ETA is hidden while a download is paused or has not received any data yet.

### Automatic output filenames

When the filename field is left blank, LinkForge first uses a filename supplied by the server's HTTP `Content-Disposition` response header (preferring RFC 5987 `filename*` when available). If the server does not provide a usable name, the existing URL-path filename fallback is retained. User-entered filenames always take precedence.
