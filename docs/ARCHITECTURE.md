# Архитектура

`q2prox-dz` — один репозиторий с общим контрактом сессии и двумя платформенными
поставками.

## Supported Product Contract (нормативный)

Поддерживается только следующий профиль:

- клиент: Q2PRO-X 1.4 by ly, сборка 2026-07-05, или точно совместимая с ней сборка;
- Dos.Zone room prefix: `q2`;
- room alias: только `q2.<owner>`, составленный из `owner` в catalog и
  подтверждённый точным совпадением в свежем lookup;
- локальный game directory: `3zb2`;
- WebRTC DataChannel: `ordered=false`;
- поиск content: выбранный game directory с fallback в `baseq2`.

Текущий catalog может не содержать отдельных полей `game`/`profile`/`ordered`;
их отсутствие допустимо. Если поле присутствует, `game` и `profile` должны быть
`3zb2`, а `ordered` — строго `false`; конфликтующая строка отбрасывается до
получения RTC/ICE credentials. Это не контракт совместимости с произвольными
Quake II forks или другими играми Dos.Zone.

## Datagram profile и безопасная диагностика

Source/unit tests обеих поставок подтверждают datagram profile: `1..1400`
bytes принимаются, `1401` и больше отбрасываются, а предел не настраивается выше
`1400`. В shutdown-диагностике gateway есть только четыре size/drop поля:
`max_udp_to_transport_datagram`, `max_transport_to_udp_datagram`,
`oversized_udp_drops` и `oversized_transport_drops`. Maxima учитывают только
принятые datagram, drop counters — только наблюдаемый исходный размер выше
предела. Payload, room JSON, alias, ICE/TURN credentials, SDP/candidates и
tokens в эту диагностику не попадают.

HumbleNet/WebRTC receive API получает caller-sized buffer. Если API сам
усекает или отвергает удалённый oversized frame, gateway не может восстановить
его исходный размер: `oversized_transport_drops=0` не доказывает отсутствие
таких frames upstream. Наблюдаемые live maxima для representative `baseq2` и
`3zb2` gameplay остаются pending до Task 16 и здесь не заявляются.

## Общий поток

1. Launcher получает список готовых публичных Quake II комнат.
2. Пользователь выбирает комнату.
3. Launcher проверяет наличие нужной карты и получает свежие room/ICE данные.
4. Coordinator запускает локальный gateway и ждёт реальный Quake II OOB
   `ping`/`ack` через WebRTC.
5. Только после readiness запускается неизменённый Q2PRO-X с
   `+set game 3zb2 +connect 127.0.0.1:<случайный-порт>`.
6. При выходе игры, закрытии комнаты или сбое launcher завершает всю сессию.

Room/ICE данные передаются через stdin, не сохраняются в аргументах,
переменных окружения или постоянных файлах. UDP слушает только loopback.

## Windows

- `windows/product/` — пользовательский launcher и portable/setup packaging;
- `windows/coordinator/` — lifecycle, content check и managed protocol;
- `windows/gateway/` — WebView2 gateway source и web assets.

Coordinator использует Windows Job Object с `KILL_ON_JOB_CLOSE`. WebView2
нужен только как поддерживаемый WebRTC runtime; portal automation не
используется.

## Linux

- `linux/` — launcher, coordinator, guardian, installer и tests;
- `linux/transport/` — используемый WebRTC-NET transport adapter.

Q2PRO-X запускается через Wine. Gateway и Wine/Q2 входят в одну управляемую
process group; guardian очищает их и session directory при смерти владельца.

## Зафиксированные решения

- [ADR 0001: Windows WebView2 backend](adr/0001-windows-webrtc-transport-backend.md)
- [ADR 0002: Windows launcher-owned lifecycle](adr/0002-launcher-owned-session-lifecycle.md)
- [ADR 0003: Linux managed session](adr/0003-linux-managed-session-and-guardian.md)

## Предлагаемое развитие

[ADR 0004](adr/0004-q2pro-x-custom-source-room-hub.md) описывает переход от
отдельного single-room picker к штатной вкладке `Custom` в server browser
Q2PRO-X и долгоживущему room hub. До прохождения указанных в ADR runtime gates
эта схема не считается реализованной и не заменяет текущий single-room поток.
