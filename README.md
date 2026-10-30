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

The executable is normally written to `build\Release\LinkForge.exe` for the command-line build, or to the selected Visual Studio CMake build directory.

## Add a download

1. Click **Add new file download**.
2. Paste one mirror URL per line. All URLs in this dialog must refer to the same file.
3. Choose the destination folder and optional output filename.
4. LinkForge checks the mirrors, compares their reported size and sampled contents, and selects a transfer mode.

When compatible URLs support HTTP byte ranges, LinkForge assigns different ranges of the file to the mirrors. If ranges are unavailable or a range implementation proves unreliable, it restarts the file from byte zero using one full-file mirror at a time. It does not append multiple complete copies.

The mirror details window shows current and average speed, bytes received, HTTP status, transfer mode, active range requests, and mirror state/error text. Mirrors can be added or removed while a download is running.

## Settings

Click **Settings** in the main window. Every setting has a tooltip explaining its effect.

| Setting | Default | Meaning |
|---|---:|---|
| Maximum simultaneous connections across all downloads | 8 | Global cap for data transfers and mirror probes |
| Maximum parallel connections to each mirror | 2 | Per-URL concurrent range-request limit |
| Gradual connection ramp-up | On | Starts each mirror with one range request and permits more after successful transfers |
| Ramp-up delay | 500 ms | Minimum wait before adding another concurrent request to a mirror |
| Retry temporary failures and throttling | On | Retries network failures and HTTP 408/425/429/5xx responses, including HTTP 509 |
| Retry delay | 5 seconds | Fallback wait when a server does not provide a `Retry-After` header |
| Maximum retries per mirror | 0 (unlimited) | Number of retries after the first attempt for a failed probe/transfer phase; zero means keep retrying until success or cancellation |
| Custom User-Agent header | On | Sends the text in the User-Agent field with each HTTP request |

The connection ramp-up is per mirror: LinkForge initially sends at most one active range request to a mirror. After successful range transfers, it can gradually increase that mirror's concurrency up to the configured limit. Other mirrors can each start with their own first request. When adaptive throttling is enabled, a 509/429 response lowers that mirror's own connection cap by one (not below one); three subsequent successful range transfers increase the cap by one until it reaches the configured maximum.

HTTP 509 is treated as temporary server-side throttling, not as proof that byte ranges are unsupported. LinkForge pauses that mirror according to the server's `Retry-After` header when present (delta-seconds or the standard HTTP-date form, capped at 24 hours); otherwise it uses the configured retry delay. Other mirrors continue while one mirror waits. If unlimited retries are enabled and every mirror remains unavailable, the download will keep waiting until a mirror responds or the user cancels it.

Settings are stored in **`LinkForge.ini` beside `LinkForge.exe`**, not in AppData or the user's profile. The executable folder must be writable for settings to be saved. Settings changes are applied to future requests; lowering the global connection cap also constrains currently active transfers. A higher worker count is picked up when a new download starts.

The User-Agent setting sends the exact configured HTTP header value. It does not automatically copy uGet's headers or guarantee that a server will treat LinkForge identically to uGet.

## Cancelling and partial files

The **Cancel download** button asks whether to keep or delete the data already downloaded:

- **Yes:** cancel and preserve the incomplete data as `<filename>.partial` in the destination folder.
- **No:** cancel and delete the temporary data.
- **Cancel:** leave the download running.

A kept `.partial` file is an incomplete artifact for inspection or manual recovery; the current version does **not** automatically resume it. In sequential fallback mode, it is normally the contiguous prefix received so far. In parallel mode, downloaded ranges can be at different offsets, so unreceived regions may remain empty in the retained partial file. It must not be treated as a completed or verified file.

When LinkForge exits normally, it asks its active download workers to stop and preserve their partial data.

## Technical notes and limitations

- Uses Win32, WinHTTP, C++20, and the Windows SDK; no Qt or embedded browser.
- HTTP and HTTPS URLs are supported.
- Parallel assembly requires all contributing mirrors to report compatible file sizes and matching prefix/middle/end samples. These checks reduce risk but are not a cryptographic proof that every byte is identical. A trusted SHA-256 checksum would provide stronger end-to-end verification; the current version does not implement one.
- Some hosts require cookies, authorization headers, referers, or other per-mirror request headers. The current GUI exposes only a User-Agent override, so such mirrors may not work without further support.
- Some hosting providers enforce per-account bandwidth limits or anti-abuse policies. A downloader cannot bypass those limits; lowering concurrency and respecting retry delays may help with temporary throttling.