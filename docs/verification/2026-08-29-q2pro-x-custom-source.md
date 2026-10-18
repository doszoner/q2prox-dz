# Проверка Custom source Q2PRO-X 1.5

Дата: 2026-08-29.

Проверен установленный клиент:

```text
~/Games/Quake2-PlayGround-2026/Q2PRO-X.exe
Q2PRO-X 1.5 by ly, 2026-08-25
SHA-256 831f42dc96e3399a8a8737943d6fde752e11f5e5f48ffc5957537c4e66cbf71d
```

Проверки выполнялись с отдельным временным `homedir`; пользовательская
установка и конфигурация не изменялись.

## Результаты

- Текстовый HTTP source с двумя loopback endpoints был получен дважды. Оба
  endpoint получили стандартный Quake II `status` с одного UDP source port.
- Составной source без HTTP,
  `quake2://127.0.0.1:38699 quake2://127.0.0.1:38700`, привёл к `status` обоих
  endpoint с одного UDP source port `57923`.
- В отдельной трассировке `status`, `getchallenge` и sequenced connect packet
  пришли с одного UDP source port `52006`.
- Ответ status с игроками Alice и Bob был разобран exact клиентом; в журнале
  появились обе стандартные player rows.
- Клиент распознал переданные `hostname`, `mapname`, `game` и `maxclients`.

## Вывод

Для первой версии room hub достаточно составного `quake2://` source. Локальный
HTTP server и ослабление привязки gateway к UDP source не нужны.

Эта проверка подтверждает контракт browser/status/connect, но не заменяет
живую проверку двух настоящих комнат, active/inactive room close и очистку
процессов из готовой поставки.
