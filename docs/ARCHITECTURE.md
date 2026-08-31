# Архитектура

`q2prox-dz` — один репозиторий с общим контрактом сессии и двумя платформенными
поставками.

## Supported Product Contract (нормативный)

Поддерживается только следующий профиль:

- клиент: Q2PRO-X 1.5 by ly, сборка 2026-08-25, или точно совместимая с ней сборка;
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
`3zb2` gameplay остаются pending до отдельной проверки ресурсов и реального
пользовательского прогона в Task 9 и здесь не заявляются.

## Встроенный браузер комнат

Launcher сразу запускает hub-режим, без отдельного окна выбора. Hub поддерживает
от нуля до двух независимых gateway и запускает один Q2PRO-X со стабильным
локальным HTTP-источником:

```text
cl_serverbrowser_custom_source=http://127.0.0.1:<source-port>/servers
```

Q2PRO-X получает через прозрачный gateway настоящее имя сервера, карту, число
сетевых клиентов, их имена и ping. Каталог опрашивается примерно раз в 15
секунд; после изменения пользователь нажимает обычный `Refresh` во вкладке
`Custom`. Q2PRO-X перезапускать не нужно. Заполненная активная комната не
вытесняется третьей, а ошибка каталога не очищает текущие комнаты.

При запуске coordinator передаёт Q2PRO-X локальный source и не передаёт команд
управления меню. Q2PRO-X использует свою обычную настройку стартового меню;
пользователь при необходимости нажимает `Esc`, открывает `Join online` →
`Custom` и нажимает `Refresh`. Принудительный `serverbrowser_open_modern` не
используется.

Закрытие или падение одного gateway не завершает Q2PRO-X и второй gateway.
После штатного таймаута либо ручного `Disconnect` пользователь выбирает другую
комнату. Выход Q2PRO-X или hard-kill hub очищает все принадлежащие ему процессы.
Room/ICE данные передаются через stdin, не сохраняются в аргументах,
переменных окружения или постоянных файлах. UDP слушает только loopback.

Число `players` из каталога Dos.Zone отражает браузерное lobby, а не всех
нативных игроков, и показывается только как `Lobby N/M`. Gateway не подменяет
Quake II status. Подробный контракт: [ADR 0004](adr/0004-q2pro-x-custom-source-room-hub.md).

Граница компонентов:

- Python/PowerShell: опрос каталога, строгая проверка и выбор двух стабильных
  слотов;
- C++ coordinator: независимые команды слотов, владение Q2PRO-X и gateway,
  локальный HTTP-источник;
- gateway: неизменённый UDP↔WebRTC transport для одной комнаты;
- Q2PRO-X: неизменённые встроенные `Custom` и `Refresh`.

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

Q2PRO-X запускается через Wine. У каждого gateway и Q2 отдельная process group;
guardian очищает весь набор по session marker и удаляет session directory при
смерти владельца.

## Зафиксированные решения

- [ADR 0001: Windows WebView2 backend](adr/0001-windows-webrtc-transport-backend.md)
- [ADR 0002: Windows launcher-owned lifecycle](adr/0002-launcher-owned-session-lifecycle.md)
- [ADR 0003: Linux managed session](adr/0003-linux-managed-session-and-guardian.md)

## Встроенный браузер

[ADR 0004](adr/0004-q2pro-x-custom-source-room-hub.md) описывает текущий путь:
штатная вкладка `Custom`, стабильный loopback HTTP source и room hub. Кандидат
`1.0.0-rc12` проверяется по указанным в ADR live, lifecycle и package gates;
фактически пройденные проверки приведены в
[rc12 verification](verification/1.0.0-rc12.md).
