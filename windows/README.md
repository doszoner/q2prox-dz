# Windows

Windows-поставка состоит из трёх небольших частей:

- `gateway/` — WebView2 UDP↔WebRTC gateway;
- `coordinator/` — lifecycle, readiness и content preflight;
- `product/` — room picker, portable/setup packaging и пользовательские README.

## Local development

Единственная release-chain инструкция находится в
[`docs/RELEASE.md`](../docs/RELEASE.md). Для локальной component-сборки нужны
MSVC, CMake, Python 3, Node.js, WebView2 SDK и locked WebRTC-NET JS/WASM release:

```powershell
cmake -S .\windows\gateway -B .\windows\gateway\build -A x64 `
  -DWEBVIEW2_SDK_DIR=C:\deps\webview2-sdk-1.0.4129.50 `
  -DWEBRTC_NET_RELEASE_DIR=C:\deps\webrtc-release
cmake --build .\windows\gateway\build --config Release
ctest --test-dir .\windows\gateway\build -C Release --output-on-failure
```

The locked external inputs are WebView2 SDK NuGet package `1.0.4129.50` and the
public WebRTC-NET repository `https://github.com/doszoner/WebRTC-NET.git` at
immutable commit `1b8c145b8fb00e8a1599ec311118cd38aad44e7a`; no tag is
required. The WebRTC-NET release directory must contain the matching
`webrtcnet.mjs` and `webrtcnet.wasm` files.

Coordinator:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\windows\coordinator\tools\build-release.ps1
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\windows\coordinator\tools\run-lifecycle-matrix.ps1
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\windows\coordinator\tools\check-reproducible.ps1 `
  -WebView2SdkDir C:\deps\webview2-sdk `
  -WebRtcNetReleaseDir C:\deps\webrtc-release
```

Packaging and publication gates are documented only in
[`docs/RELEASE.md`](../docs/RELEASE.md).
