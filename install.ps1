# Installs Bathos for this user (%LOCALAPPDATA%\Programs\Bathos, Start menu and Desktop shortcuts named "Atelier - Bathos"), through install.bat.
& cmd /c "`"$PSScriptRoot\install.bat`" $args"
exit $LASTEXITCODE
