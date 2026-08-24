$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing

$app = Split-Path -Parent $MyInvocation.MyCommand.Path
. (Join-Path $app 'catalog.ps1')
$state = Join-Path $env:LOCALAPPDATA 'Q2PRO-X Dos.Zone\state'
$q2PathFile = Join-Path $state 'q2-path.txt'
$coordinator = Join-Path $app 'q2dz-session.exe'
$contentCheck = Join-Path $app 'q2dz-content-check.exe'
$gateway = Join-Path $app 'gateway\q2-doszone-webview2-gateway.exe'
foreach ($path in $coordinator,$contentCheck,$gateway) {
  if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Installation is incomplete: $path" }
}

if (Get-Process 'Q2PRO-X' -ErrorAction SilentlyContinue) {
  [Windows.Forms.MessageBox]::Show('Close the running Q2PRO-X before joining a Dos.Zone room.','Q2PRO-X - Dos.Zone') | Out-Null
  exit 4
}

$q2 = if (Test-Path $q2PathFile) { (Get-Content $q2PathFile -Raw).Trim() } else { '' }
if (-not $q2 -or -not (Test-Path -LiteralPath $q2 -PathType Leaf)) {
  $picker = New-Object Windows.Forms.OpenFileDialog
  $picker.Title = 'Select your installed Q2PRO-X.exe'
  $picker.Filter = 'Q2PRO-X|Q2PRO-X.exe'
  if ($picker.ShowDialog() -ne [Windows.Forms.DialogResult]::OK) { exit 1 }
  $q2 = $picker.FileName
  New-Item -ItemType Directory -Force $state | Out-Null
  Set-Content -LiteralPath $q2PathFile -Value $q2 -Encoding UTF8
}

$catalog = Get-Q2DzJson $Q2DZ_ROOM_ENDPOINT
$lookupForList = Get-Q2DzJson $Q2DZ_LOOKUP_ENDPOINT
$rooms = @(Get-Q2DzRegisteredRooms -Catalog $catalog -LookupPayload $lookupForList)
if ($rooms.Count -eq 0) {
  [Windows.Forms.MessageBox]::Show('There are no joinable Quake II rooms right now.','Q2PRO-X - Dos.Zone') | Out-Null
  exit 0
}

$form = New-Object Windows.Forms.Form
$form.Text = 'Q2PRO-X - Dos.Zone rooms'
$form.Size = New-Object Drawing.Size(620,420)
$form.StartPosition = 'CenterScreen'
$list = New-Object Windows.Forms.ListBox
$list.Location = New-Object Drawing.Point(12,12)
$list.Size = New-Object Drawing.Size(580,320)
$list.DisplayMember = 'Display'
foreach ($room in $rooms) {
  [void]$list.Items.Add([pscustomobject]@{
    Display = "$($room.createSettings.name)   [$($room.players)/$($room.createSettings.maxPlayers)]"
    Room = $room
  })
}
$join = New-Object Windows.Forms.Button
$join.Text = 'Join'
$join.Location = New-Object Drawing.Point(492,342)
$join.Size = New-Object Drawing.Size(100,30)
$join.Enabled = $false
$list.Add_SelectedIndexChanged({ $join.Enabled = $list.SelectedIndex -ge 0 })
$join.Add_Click({ $form.DialogResult = [Windows.Forms.DialogResult]::OK; $form.Close() })
$list.Add_DoubleClick({ if ($list.SelectedIndex -ge 0) { $join.PerformClick() } })
$form.Controls.AddRange(@($list,$join))
if ($form.ShowDialog() -ne [Windows.Forms.DialogResult]::OK) { exit 0 }
$selected = $list.SelectedItem.Room

# Revalidate every volatile contract immediately before joining.
$fresh = Get-Q2DzJson $Q2DZ_ROOM_ENDPOINT
$freshRooms = @(Get-Q2DzJoinableRooms $fresh | Where-Object { $_.room -ceq $selected.room })
if ($freshRooms.Count -ne 1) { throw 'The selected room is no longer available.' }
$selected = $freshRooms[0]
$transport = Get-Q2DzSelectedTransport -Room $selected
$alias = $transport.Alias
$rtc = $transport.Rtc

$map = [string]$selected.createSettings.map
& $contentCheck --root (Split-Path -Parent $q2) --game $Q2DZ_GAME --map $map | Out-Null
if ($LASTEXITCODE -ne 0) {
  [Windows.Forms.MessageBox]::Show("Map '$map' is missing from this Q2PRO-X installation.",'Q2PRO-X - Dos.Zone') | Out-Null
  exit 3
}

$config = @{
  version = 1; roomId = $selected.room; alias = $alias; ordered = $Q2DZ_ORDERED
  iceServers = $rtc.iceservers
} | ConvertTo-Json -Depth 8 -Compress
$config | & $coordinator --gateway $gateway --q2 $q2 --session-root (Join-Path $state 'sessions')
$code = $LASTEXITCODE
$config = $null; $rtc = $null; $transport = $null; [GC]::Collect()
exit $code
