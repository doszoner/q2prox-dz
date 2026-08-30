# Windows

Windows-поставка состоит из трёх небольших частей:

- `gateway/` — WebView2 UDP↔WebRTC gateway;
- `coordinator/` — lifecycle, readiness и content preflight;
- `product/` — catalog launcher, portable/setup packaging и пользовательские README.

## Текущий режим

`q2dz-launch.ps1` получает и проверяет каталог Dos.Zone, поддерживает до двух
комнат и передаёт независимые команды слотов координатору через `stdin`.
Координатор запускает один gateway на комнату и один неизменённый Q2PRO-X со
стабильным локальным HTTP-источником для штатной вкладки `Custom`. Отдельного
окна выбора комнаты нет.

Launcher опрашивает каталог примерно раз в 15 секунд. Пользователь нажимает
обычную кнопку `Refresh` в `Custom`, чтобы увидеть новую комнату после
очередного успешного опроса или закрытую после двух опросов, в которых её уже
нет. Q2PRO-X перезапускать не нужно. Заполненная активная комната сохраняется,
а третья комната не вытесняет одну из двух уже выбранных. Ошибка каталога
сохраняет текущие комнаты. Ручное редактирование CFG, Favorites или аргументов
запуска не требуется.

При закрытии одной комнаты удаляется только её gateway; Q2PRO-X и остальная
комната продолжают работать. Выход Q2PRO-X или жёсткое завершение владельца
останавливает всю сессию. Отдельный внутренний очиститель ждёт завершения
coordinator и удаляет его session directory даже при жёстком завершении.

Граница компонентов:

- PowerShell: опрос, проверка каталога и выбор двух стабильных слотов;
- C++ coordinator: владение Q2PRO-X/gateway и локальный HTTP-источник;
- gateway: существующий transport для одной комнаты;
- Q2PRO-X: штатные `Custom` и `Refresh`.

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
