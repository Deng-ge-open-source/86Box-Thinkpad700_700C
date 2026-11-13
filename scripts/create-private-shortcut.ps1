$ErrorActionPreference = 'Stop'
$desktop = [Environment]::GetFolderPath('Desktop')
$shortcutPath = Join-Path $desktop 'ThinkPad 700C 私人交接版.lnk'
$shortcut = (New-Object -ComObject WScript.Shell).CreateShortcut($shortcutPath)
$shortcut.TargetPath = Join-Path $PSScriptRoot '86Box.exe'
$shortcut.WorkingDirectory = $PSScriptRoot
$shortcut.Arguments = '-N -W -F -R "' + (Join-Path $PSScriptRoot 'runtime\roms') + '" -P "' + (Join-Path $PSScriptRoot 'runtime\ThinkPad 700C') + '"'
$shortcut.IconLocation = (Join-Path $PSScriptRoot '86Box.exe') + ',0'
$shortcut.Description = 'ThinkPad 700C private handoff: color fullscreen, Reference Diskette first'
$shortcut.Save()
Write-Output "Created: $shortcutPath"
