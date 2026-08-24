# ADR 0001: Windows WebRTC transport backend

- **Status:** Accepted
- **Date:** 2026-08-22
- **Owner:** doszoner

## Context

Q2PRO-X должен подключаться к Dos.Zone без изменения игры и без видимого
браузерного flow. Проверенный Linux transport использует WebRTC-NET с Chromium
WebRTC backend, который напрямую не поставляется для современной Windows.

Собственный Windows-порт Chromium/libwebrtc потребовал бы поддержки большого
toolchain, регулярных security updates и отдельной совместимости с Dos.Zone.

## Decision

Windows gateway использует скрытый Microsoft Edge WebView2 host с упакованными
`webrtcnet.mjs` и `webrtcnet.wasm`. WebView2 служит только WebRTC runtime:

- portal pages, cookies и browser automation не используются;
- navigation, DevTools и внешние окна запрещены;
- Q2PRO-X общается только с UDP-портом на `127.0.0.1`;
- lifecycle WebView2, gateway и Q2PRO-X принадлежит session coordinator;
- session user-data directory изолируется и удаляется после завершения;
- room/ICE данные не передаются через arguments, environment или файлы.

Используется установленный Evergreen WebView2 Runtime. Fixed Version Runtime не
включается в поставку: его размер и security-update ownership здесь не нужны.

## Verification contract

Один exact Windows artifact должен проходить:

1. clean Windows x64 startup;
2. signaling, fresh ICE и alias resolution;
3. Quake II OOB `ping`/`ack`, protocol handshake и вход в карту;
4. движение/стрельбу в реальной комнате;
5. normal exit, crash, forced termination и room-close cleanup;
6. отсутствие credentials в archive, args, env и logs;
7. reproducible build и полный license/notice set.

`DataChannel open` без входа в игру не считается успешной проверкой.

## Rejected alternatives

- **Native Chromium/libwebrtc port:** несоразмерная build и update ownership.
- **Устаревший Windows WebRTC tag:** неприемлемый security debt.
- **WSL, Wine или ручные DLL:** плохая пользовательская поставка для Windows.
- **Public UDP gateway:** отдельный сервис с abuse и availability ownership.

## Consequences

Поставка зависит от поддерживаемого WebView2 Runtime и содержит небольшой
native↔web IPC boundary. Транспорт не требует изменений Q2PRO-X или Dos.Zone.
Широкий публичный Windows release дополнительно требует Authenticode; это не
меняет выбранный backend.
