# ADR 0004: комнаты Dos.Zone во встроенном server browser Q2PRO-X

- **Status:** Proposed
- **Date:** 2026-08-23
- **Related:** [Архитектура](../ARCHITECTURE.md),
  [ADR 0002](0002-launcher-owned-session-lifecycle.md),
  [ADR 0003](0003-linux-managed-session-and-guardian.md)

## Контекст

Текущая поставка показывает комнаты в отдельном launcher, поднимает один
gateway и запускает Q2PRO-X с
`+set game 3zb2 +connect 127.0.0.1:<port>`. Пока Q2PRO-X
работает, эта сессия привязана к одной комнате. Для перехода в другую комнату
пользователь должен полностью закрыть игру и повторно запустить launcher.

Q2PRO-X 1.4 уже имеет подходящий штатный интерфейс:

- современный server browser со вкладками `q2servers.com`, `Favorites`, `LAN`
  и `Custom`;
- локальную адресную книгу `adr0..adr15` для `Favorites`;
- `cl_serverbrowser_custom_source` для отдельного HTTP/file source;
- обычный Quake II `status` для получения hostname, mod, map, players и ping.

На проверенной пользовательской установке четыре PlayGround-сервера появились
в `Favorites` только из локальных `adr0..adr3`; изменения Q2PRO-X со стороны
maintainer не потребовались. Следовательно, базовая интеграция Dos.Zone также
не требует модификации Q2PRO-X или участия его maintainer.

Подтверждено локально, что меню Q2PRO-X предлагает для `Custom` значение вида
`+http://example.com/?raw=2`. Но точный формат ответа, повторное чтение source
по `Refresh` и переход от browser query к игровому `connect` через один gateway
ещё не проверены end-to-end. Ниже они зафиксированы как обязательные gates, а
не как готовые свойства текущей поставки.

`Favorites` использовать для Dos.Zone нельзя: это заняло бы пользовательские
слоты, смешало динамические комнаты с личными серверами и создало бы конфликт
владения конфигурацией. `Custom` уже даёт отдельную штатную поверхность.

## Решение

Целевое подключение реализуется через launcher-owned **room hub** и вкладку
`Custom` Q2PRO-X. Текущий single-room flow остаётся поставляемым baseline до
прохождения gates этого ADR.

### Пользовательский сценарий

1. Пользователь запускает `Q2PRO-X — Dos.Zone`.
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
  Room C -> 127.0.0.1:31003 -> gateway -> Dos.Zone Room C
```

Hub отдаёт Q2PRO-X только loopback endpoints. Он не генерирует synthetic
Quake II status. Browser отправляет обычный status-запрос на выбранный
loopback endpoint, gateway передаёт его в настоящую комнату, а Q2PRO-X получает
фактические hostname, mod, map, players и end-to-end ping.

### Custom source contract

Hub слушает HTTP только на случайном loopback high port. Launcher передаёт
актуальный URL Q2PRO-X через штатный `cl_serverbrowser_custom_source` при
запуске add-on; URL не является секретом. Installer и hub не редактируют CFG
самостоятельно. Архивирует ли Q2PRO-X это значение при autosave и требуется ли
явно очищать его при обычном запуске игры, определяет runtime gate.

Предпочтительный wire format — используемый Q2PRO-X `raw=2`: тело состоит из
последовательных шестибайтовых IPv4 endpoints (четыре байта адреса и
двухбайтовый UDP port). Exact byte order и URI syntax должны быть зафиксированы
runtime-тестом до реализации. Текстовый newline-separated source допустим как
fallback только после такого же exact runtime proof.

Source содержит только gateways со статусом ready. При обычном `Refresh`
Q2PRO-X заново читает source и опрашивает endpoints. Hub обновляет каталог не
чаще одного раза в 30 секунд и немедленно удаляет endpoint после подтверждённого
закрытия комнаты.

Существующие gateways захватывают первый loopback UDP source и отвергают второй.
Поэтому первая проверка должна доказать, что exact Q2PRO-X использует один и тот
же UDP source socket для `status` и последующего `connect`. Если это неверно,
реализация не должна ослаблять source isolation наугад: ADR возвращается на
пересмотр с фактической трассировкой пакетов.

### Общий и платформенный слои

Функция является общим продуктовым слоем, но не новым transport backend.

Общий контракт и одинаковое поведение на обеих платформах:

- получение и валидация каталога Dos.Zone;
- room registry и соответствие room ID → loopback UDP endpoint;
- reconcile добавленных, сохранённых и удалённых комнат;
- локальный Custom source;
- правила readiness, refresh, room close и shutdown;
- redaction, exit reasons и acceptance tests.

Платформенные адаптеры:

- Windows запускает существующие WebView2 gateways и использует Job Object;
- Linux запускает существующие native WebRTC-NET gateways и использует process
  group/guardian;
- Windows запускает native `Q2PRO-X.exe`, Linux — тот же exe через Wine;
- установка и обнаружение Q2PRO-X остаются раздельными.

Для первой реализации не создаётся общий кроссплатформенный runtime framework.
PowerShell на Windows и Python на Linux могут реализовать небольшую orchestration
часть отдельно, следуя одному контракту и общим fixtures. Общий C++ hub имеет
смысл только если фактическое дублирование или расхождение поведения станет
дороже двух небольших реализаций.

### Lifecycle

Hub владеет одной запущенной Q2PRO-X и набором gateways. Закрытие одной комнаты
завершает только её gateway и удаляет одну строку source; оно не завершает Q2 и
не затрагивает другие комнаты. Закрытие активной комнаты приводит к обычному
разрыву соединения Q2, после чего пользователь может выбрать другую комнату.

Выход Q2, закрытие hub или crash владельца очищают весь принадлежащий add-on
process tree. Windows сохраняет `KILL_ON_JOB_CLOSE`, Linux — guardian. На каждую
WebView2-сессию используется отдельный user-data directory.

### Content compatibility

До публикации endpoint hub проверяет наличие требуемой карты существующим
content checker. Несовместимая комната не показывается в `Custom`; причина
попадает в redacted diagnostic log. Если совместимых комнат нет, launcher
показывает понятное сообщение до запуска Q2PRO-X.

## Security invariants

- HTTP и UDP слушают только `127.0.0.1`.
- Room alias, ICE/TURN credentials, SDP и candidates не попадают в source,
  arguments, environment, CFG или постоянные файлы.
- Fresh room configuration передаётся каждому gateway только через stdin.
- В source нет room credentials и внешних адресов Dos.Zone — только loopback.
- Hub ограничивает размер и схему внешнего каталога; допустимый предел
  одновременно подготовленных gateways устанавливается после resource gate.
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

Уже реализован и остаётся rollback path. Он проще, но не позволяет штатно
сменить комнату без перезапуска Q2PRO-X. Не является целевым UX.

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

- одновременно работает один gateway на каждую опубликованную ready-комнату;
- Windows может держать несколько WebView2 process trees;
- room hub становится долгоживущим владельцем Q2 и gateways;
- перед заменой single-room flow нужны exact runtime и resource gates.

## Обязательные gates

1. **Custom source:** exact Q2PRO-X читает localhost source, показывает все
   endpoints и обновляет их по `Refresh`; зафиксированы wire format, URI syntax
   и поведение cvar autosave.
2. **Real status:** две комнаты одновременно показывают фактические hostname,
   mod, map, players и измеренный через transport ping без synthetic response;
   packet trace подтверждает один UDP source для `status` и `connect`.
3. **Switch:** Q2 входит в Room A, выполняет `Disconnect`, затем входит в Room B
   без завершения процесса Q2PRO-X; движение и выстрел видны владельцу Room B.
4. **Isolation:** закрытие неактивной Room A не влияет на игру в Room B;
   закрытие активной комнаты не завершает hub или Q2.
5. **Refresh:** появление и закрытие комнат отражаются после bounded refresh,
   stale endpoint не остаётся доступным.
6. **Lifecycle:** normal exit, Q2 crash, gateway crash и hub hard-kill не
   оставляют gateway, WebView2/Wine или session data.
7. **Security:** source, args, env, CFG и redacted logs не содержат room/ICE
   credentials.
8. **Resources:** Windows и Linux измерены при 1, 4 и 8 ready-комнатах; по
   результату устанавливается и документируется operational cap.
9. **Packaging:** portable/setup/Linux package проходят smoke и live switch
   именно из заново распакованных release artifacts.

До прохождения gates 1–8 этот ADR остаётся `Proposed`, а существующий
single-room launcher — единственным подтверждённым release path. Rollback после
внедрения означает возврат launcher к одной комнате и
`+set game 3zb2 +connect`; gateways и transport backend при этом не меняются.
