# ADR 0004: комнаты Dos.Zone во встроенном server browser Q2PRO-X

- **Status:** Proposed
- **Date:** 2026-08-23
- **Related:** [Архитектура](../ARCHITECTURE.md),
  [ADR 0002](0002-launcher-owned-session-lifecycle.md),
  [ADR 0003](0003-linux-managed-session-and-guardian.md)

## Контекст

Поставка до rc9 показывала комнаты в отдельном launcher, поднимала один
gateway и запускала Q2PRO-X с
`+set game 3zb2 +connect 127.0.0.1:<port>`. Пока Q2PRO-X
работал, эта сессия была привязана к одной комнате. Для перехода в другую
комнату пользователь должен был полностью закрыть игру и повторно запустить
launcher.

Q2PRO-X 1.5 by ly от 2026-08-25 уже имеет подходящий штатный интерфейс:

- современный server browser со вкладками `q2servers.com`, `Favorites`, `LAN`
  и `Custom`;
- локальную адресную книгу `adr0..adr15` для `Favorites`;
- `cl_serverbrowser_custom_source` для составного списка `quake2://`;
- обычный Quake II `status` для получения hostname, mod, map, players и ping.

На проверенной пользовательской установке четыре PlayGround-сервера появились
в `Favorites` только из локальных `adr0..adr3`; изменения Q2PRO-X со стороны
maintainer не потребовались. Следовательно, базовая интеграция Dos.Zone также
не требует модификации Q2PRO-X или участия его maintainer.

Подтверждено на exact Q2PRO-X 1.5, SHA-256
`831f42dc96e3399a8a8737943d6fde752e11f5e5f48ffc5957537c4e66cbf71d`, что
составной source вида
`quake2://127.0.0.1:31001 quake2://127.0.0.1:31002` опрашивает оба endpoint.
`status`, `getchallenge` и `connect` используют один UDP source. Свидетельства
зафиксированы в
[проверке exact клиента](../verification/2026-08-29-q2pro-x-custom-source.md).

`Favorites` использовать для Dos.Zone нельзя: это заняло бы пользовательские
слоты, смешало динамические комнаты с личными серверами и создало бы конфликт
владения конфигурацией. `Custom` уже даёт отдельную штатную поверхность.

## Решение

Целевое пользовательское подключение реализуется только через launcher-owned
**room hub** и вкладку `Custom` Q2PRO-X. Отдельный picker и single-room ветка
координатора удаляются, чтобы не поддерживать два интерфейса и два lifecycle.
Проверенные rc8 artifacts сохраняют архивный rollback до прохождения gates rc9.

### Пользовательский сценарий

1. Пользователь запускает `Q2PRO-X — Dos.Zone` без предварительного picker.
2. Room hub получает готовые публичные Quake II комнаты и свежие RTC данные
   через те же Dos.Zone endpoints, которые уже использует launcher.
3. Hub проверяет локальное наличие карты и для каждой совместимой комнаты
   поднимает отдельный gateway на уникальном
   `127.0.0.1:<udp-port>`.
4. Только gateway, прошедший настоящий Quake II OOB `ping`/`ack`, появляется в
   локальном `Custom` source.
5. Q2PRO-X запускается один раз и открывает встроенный server browser.
6. Пользователь выбирает комнату обычным способом.
7. Для смены комнаты пользователь выполняет `Disconnect`, снова открывает
   browser и выбирает другую строку. Q2PRO-X не перезапускается.
8. При выходе из Q2PRO-X hub завершает все gateway и временные данные.

```text
Q2PRO-X Custom source
  Room A -> 127.0.0.1:31001 -> gateway -> Dos.Zone Room A
  Room B -> 127.0.0.1:31002 -> gateway -> Dos.Zone Room B
```

Hub отдаёт Q2PRO-X только loopback endpoints. Он не генерирует synthetic
Quake II status. Browser отправляет обычный status-запрос на выбранный
loopback endpoint, gateway передаёт его в настоящую комнату, а Q2PRO-X получает
фактические hostname, mod, map, players и end-to-end ping.

### Custom source contract

Первая версия не поднимает HTTP server и не пишет source-файл. Launcher
передаёт Q2PRO-X составной список готовых loopback endpoints через
`cl_serverbrowser_custom_source`. В snapshot попадает не более двух комнат.
`Refresh` повторно опрашивает те же endpoints. Новые комнаты появляются после
следующего запуска hub; закрытая комната может оставаться недоступной строкой
до этого момента.

Q2PRO-X может сохранить несекретный loopback source штатным autosave. Add-on не
редактирует CFG и не меняет `cl_serverbrowser_default_source`: пользователь
выбирает вкладку `Custom`. Diff изолированного профиля остаётся release gate.

Существующие gateways захватывают первый loopback UDP source и отвергают второй.
Поэтому первая проверка должна доказать, что exact Q2PRO-X использует один и тот
же UDP source socket для `status` и последующего `connect`. Если это неверно,
реализация не должна ослаблять source isolation наугад: ADR возвращается на
пересмотр с фактической трассировкой пакетов.

### Общий и платформенный слои

Функция является общим продуктовым слоем, но не новым transport backend.

Общий контракт и одинаковое поведение на обеих платформах:

- получение и валидация каталога Dos.Zone;
- snapshot до двух комнат и соответствие room ID → loopback UDP endpoint;
- составной loopback Custom source;
- правила readiness, refresh, room close и shutdown;
- redaction, exit reasons и acceptance tests.

Платформенные адаптеры:

- Windows запускает существующие WebView2 gateways и использует Job Object;
- Linux запускает существующие native WebRTC-NET gateways и использует process
  group/guardian;
- Windows запускает native `Q2PRO-X.exe`, Linux — тот же exe через Wine;
- установка и обнаружение Q2PRO-X остаются раздельными.

Существующий координатор на каждой платформе становится hub coordinator.
PowerShell и Python только собирают и валидируют снимок комнат; они не
становятся границей владения процессами.

### Lifecycle

Hub владеет одной запущенной Q2PRO-X и набором gateways. У каждого gateway свои
config/event/owner pipes и WebView2 directory. Windows использует корневой Job
Object для всей сессии и вложенный Job для дерева каждого gateway: закрытие
вложенного Job убирает только соответствующие gateway/WebView2 процессы. На
Linux у каждого gateway и Q2 отдельная process group; guardian
очищает весь набор по существующему session marker. Закрытие или падение одной
комнаты завершает только её gateway и не затрагивает Q2 и другие комнаты. При
закрытии активной комнаты Q2 остаётся запущен; соединение
завершается по штатному таймауту либо пользователь выполняет `Disconnect`,
после чего выбирает другую комнату.

Выход Q2, закрытие hub или crash владельца очищают весь принадлежащий add-on
process tree. Windows сохраняет `KILL_ON_JOB_CLOSE`, Linux — guardian. На каждую
WebView2-сессию используется отдельный user-data directory.

### Content compatibility

До публикации endpoint hub проверяет наличие требуемой карты существующим
content checker. Несовместимая комната не показывается в `Custom`; причина
попадает в redacted diagnostic log. Если совместимых комнат нет, launcher
показывает понятное сообщение до запуска Q2PRO-X.

## Security invariants

- UDP gateway слушают только `127.0.0.1`.
- Room alias, ICE/TURN credentials, SDP и candidates не попадают в source,
  arguments, environment, CFG или постоянные файлы.
- Fresh room configuration передаётся каждому gateway только через stdin.
- В source нет room credentials и внешних адресов Dos.Zone — только loopback.
- Hub ограничивает размер и схему внешнего каталога и готовит не более двух
  gateway.
- Installer не занимает `adr0..adr15`, не создаёт `servers.lst` и не изменяет
  Favorites пользователя.

## Не входит в решение

- изменение `Q2PRO-X.exe` или добавление отдельной вкладки силами maintainer;
- создание или администрирование комнат Dos.Zone;
- browser/portal automation;
- synthetic server status или фиктивные player rows;
- общий внешний UDP gateway;
- оптимизация нескольких комнат в один WebView2/native transport process до
  появления измеренной проблемы ресурсов.

## Альтернативы

### `adr0..adr15` / Favorites

Технически работает и подтверждено текущей установкой, но занимает личные
слоты, имеет лимит 16 и смешивает владельцев конфигурации. Отклонено.

### `servers.lst`

Не требует HTTP listener, но требует писать внутрь virtual filesystem Q2,
смешивает комнаты с Favorites и хуже обновляется во время работы. Отклонено.

### Отдельный room picker и `+set game 3zb2 +connect`

Удалён из текущего кода. Он сохраняется только в исторических rc8 artifacts:
два режима создавали бы лишнюю поддержку и разный пользовательский опыт.

### First-class provider внутри Q2PRO-X

Дал бы отдельную вкладку `Dos.Zone`, но требует изменений и поддержки другого
проекта. Для базовой интеграции не нужен. Может быть рассмотрен позже без
изменения room-hub transport architecture.

### Один многокомнатный gateway

Уменьшил бы число процессов, но потребовал бы новый multiplexing contract и
изменение уже проверенных gateways. Отложен до измеренного resource bottleneck.

## Последствия

Положительные:

- комнаты Dos.Zone становятся обычными серверами в знакомом интерфейсе;
- Q2PRO-X запускается один раз и переключает комнаты без restart;
- Favorites и обычный shortcut игры остаются независимыми;
- реальные server status и ping не подменяются;
- Windows и Linux сохраняют один пользовательский контракт.

Компромиссы:

- одновременно работает не более двух gateway;
- Windows может держать несколько WebView2 process trees;
- room hub становится долгоживущим владельцем Q2 и gateways;
- перед распространением rc9 нужны exact runtime и resource gates.

## Обязательные gates

1. **Custom source:** exact Q2PRO-X показывает оба составных `quake2://`
   endpoint; diff normal-exit/crash профиля ограничен штатным autosave и
   несекретным loopback source.
2. **Real status:** две комнаты одновременно показывают фактические hostname,
   mod, map, players и измеренный через transport ping без synthetic response;
   packet trace подтверждает один UDP source для `status` и `connect`.
3. **Switch:** Q2 входит в Room A, выполняет `Disconnect`, затем входит в Room B
   без завершения процесса Q2PRO-X; движение и выстрел видны владельцу Room B.
4. **Room close:** закрытие активной и неактивной комнат завершает только
   соответствующий gateway; Q2 и второй gateway остаются живы, после чего
   выполняется вход во вторую комнату.
5. **Lifecycle:** normal exit, Q2 crash, gateway crash и hub hard-kill не
   оставляют gateway, WebView2/Wine или session data.
6. **Security:** source, args, env, CFG и redacted logs не содержат room/ICE
   credentials.
7. **Resources:** Windows и Linux измерены при одной и двух ready-комнатах;
   operational cap первой версии равен двум.
8. **Packaging:** portable/setup/Linux package проходят smoke и live switch
   именно из заново распакованных release artifacts.

До прохождения gates 1–8 этот ADR остаётся `Proposed`, а rc9 не считается
готовым к распространению. Rollback означает возврат к rc8; gateways и
transport backend при этом не меняются.
