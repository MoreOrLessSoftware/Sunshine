' Starts the "Nyrna Resume" scheduled task. Opened by the notification's Resume button through the
' sunshine-nyrna: link; wscript.exe runs it without a console window.
Set service = CreateObject("Schedule.Service")
service.Connect
service.GetFolder("\Sunshine").GetTask("Nyrna Resume").Run ""
