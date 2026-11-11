# Installs Bathos for this user (%LOCALAPPDATA%\Programs\Bathos, Start menu + Desktop shortcuts), through install.bat.
& cmd /c "`"$PSScriptRoot\install.bat`" $args"
exit $LASTEXITCODE
