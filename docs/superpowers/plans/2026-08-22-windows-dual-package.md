# Windows Dual Package Implementation Plan

> Исторический документ; не инструкция к текущей версии. Перенесён 2026-10-18.
> Отметки задач и результаты относятся к исходной дате; проверки сейчас не повторялись.
> Пути прототипов и локальных evidence могут отсутствовать. Актуальное состояние — в индексе docs/README.md.

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Produce portable and per-user setup Windows ZIPs from one exact, whitelisted RC payload.

**Architecture:** Keep the existing WebView2 gateway, coordinator, content checker and PowerShell room launcher unchanged. A single PowerShell packager validates and copies an explicit payload allowlist into two layouts; `Run.cmd` is the only new portable runtime entry point.

**Tech Stack:** PowerShell 5.1, Windows CMD, ZIP, SHA-256, existing Windows RC binaries.

---

### Task 1: Package contract test

**Files:**
- Create: `product/tests/package-windows.tests.ps1`

- [x] Create a synthetic allowed payload, run `product/package-windows.ps1`, expand both ZIPs and assert their expected entry points.
- [x] Assert every runtime payload file has the same SHA-256 in portable root and setup `payload/`.
- [x] Assert an unexpected file in the input payload makes packaging fail.
- [x] Run the test before implementation and record the expected missing-script failure.

### Task 2: Minimal dual packager

**Files:**
- Create: `product/Run-Portable.cmd`
- Create: `product/README-PORTABLE.txt`
- Create: `product/package-windows.ps1`
- Modify: `product/README.md`

- [x] Add `Run-Portable.cmd` which invokes the existing adjacent `q2dz-launch.ps1` with Windows PowerShell and no profile.
- [x] Add a short portable README: extract, run, select existing Q2PRO-X, choose room, delete folder to remove.
- [x] Implement a fail-closed payload allowlist, internal SHA256 manifests, two ZIPs and adjacent archive SHA-256 files.
- [x] Run `product/tests/package-windows.tests.ps1` and obtain PASS.

### Task 3: Exact Windows artifacts

**Files:**
- Output only: `dist/Q2PRO-X-DosZone-1.0.0-rc3-win-x64-portable.zip`
- Output only: `dist/Q2PRO-X-DosZone-1.0.0-rc3-win-x64-setup.zip`

- [x] Use the already validated RC3 payload as the single packaging input.
- [x] Verify both external SHA-256 files and every internal manifest.
- [x] Verify the two extracted payloads are byte-identical.
- [x] On Windows, run portable launcher smoke and setup install/uninstall smoke; do not change Q2PRO-X, CFG or PAK.

### Task 4: Status and handoff

**Files:**
- Modify: `README.md`
- Modify: `IMPLEMENTATION-STATUS-20260822.md`
- Modify: `docs/REPOSITORY-AND-MULTIPLATFORM-DELIVERY.md`

- [x] Record both artifact names, hashes, exact shared-payload proof and Windows smoke boundary.
- [x] Run the package contract tests on Windows PowerShell 5.1 and in the repository PowerShell container; verify both archives and run `git diff --check`.
- [x] Leave changes uncommitted until the user separately requests a commit.
