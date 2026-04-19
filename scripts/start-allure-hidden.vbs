' Hidden launcher for the Allure server, invoked by the Scheduled Task.
' WshShell.Run with windowStyle=0 -> no console window.
' bWaitOnReturn=True -> blocks until bun exits, propagating its exit code
' so Task Scheduler's "restart on failure" kicks in if the server dies.
Dim WshShell
Set WshShell = CreateObject("WScript.Shell")
WshShell.CurrentDirectory = "C:\GhostCOM-src"
WScript.Quit WshShell.Run("""C:\Users\test\.bun\bin\bun.exe"" ""C:\GhostCOM-src\scripts\allure-server.mjs""", 0, True)
