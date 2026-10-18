# Архив проекта

Сохранены исходные проектные документы из локальных архивов 2026-08-23/24.
Публичные копии отредактированы: личные пути заменены, локальные ссылки
помечены как недоступные. Контрольные суммы ниже относятся к оригиналам,
а не к отредактированным копиям. Исходные файлы сохранены вне репозитория.

Это история решений, а не текущий backlog: первоначальный однокомнатный
picker заменён общим gateway и `Custom`, а spike WebView2 заменён продуктовым
backend. Предложение VPS не принято и не разрешает открывать локальный
шлюз в интернет. Текущий контракт описан в [ADR 0004](../adr/0004-q2pro-x-custom-source-room-hub.md).

| Публичный документ | Исходный файл | SHA-256 оригинала |
| --- | --- | --- |
| [2026-08-22-gateway-design.md](2026-08-22-gateway-design.md) | `GATEWAY-DESIGN.md` | `ff7a76a54273c07b77079c269aacfb22c829b3c05b2e41a8aac5fff86a01cff2` |
| [2026-08-22-windows-dual-package.md](../superpowers/plans/2026-08-22-windows-dual-package.md) | `2026-08-22-windows-dual-package.md` | `edba13c22f854ced0f9299bc7be14f9fe9e775896e6bdb93b1829393310cea94` |
| [2026-08-22-windows-webview2-platform-spike.md](../superpowers/plans/2026-08-22-windows-webview2-platform-spike.md) | `2026-08-22-windows-webview2-platform-spike.md` | `de7a4a8707ddde085509e95dbd5d2b70da2624e872b69fa4e73e3b665f4dfeba` |
| [2026-08-23-q2prox-dz-review-remediation-and-room-hub.md](../superpowers/plans/2026-08-23-q2prox-dz-review-remediation-and-room-hub.md) | `2026-08-23-q2prox-dz-review-remediation-and-room-hub.md` | `18e9e637744264082f82b01e0d3cc1fc6067d5d27144170ff835848746553e52` |
| [2026-08-23-hosted-vps-gateway-proposal.md](2026-08-23-hosted-vps-gateway-proposal.md) | `0005-hosted-vps-gateway.md` | `7a975bfbff74c5d93568f7b32bce88435083affee7f098b7178b273116021030` |

Также опубликованы сокращённые диагностические отчёты в `docs/verification`
и [сводка исследования пропажи комнат](../research/2026-10-01-rooms-vanish.md).
Полные отчёты с персональными данными, журналы, reviewer prompts, диалоги,
чатовые EXE/RAR и примеры стороннего кода без установленного происхождения
не переносятся. Дубли ADR 0001–0003 не импортируются повторно.

Спецификации соседних игр, browser Q2PRO и серверного coturn принадлежат
своим сервисам. В этом репозитории находятся только две service-owned задачи
из общего рабочего каталога: [OpenSpec](../../openspec/README.md).
