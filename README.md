# mira-winapp-shims

Clean-room shim DLLs and registry settings that let Windows-only apps (Microsoft 365 first, Adobe Creative Cloud and Autodesk later. Other apps may be added as needed) install and run on stock GE-Proton. Used by [Mira](https://github.com/Mira-Launcher/Mira) could also be used separately.

## Layout

- `shims/` shim DLL sources, built with MinGW (`make`)
- `shims/qmgr/` a BITS service that downloads over parallel keep-alive HTTP connections and takes file ranges (Wine's cannot, so Office's Click-to-Run falls back to a slow transport). Written from the public BITS interfaces.
- `shims/wintrust/` checks AppX/MSIX signatures at their timestamp, as Windows does, and loads binary registry hives for RegLoadAppKeyW/RegLoadKeyW, which Wine can't. Autodesk's installer needs both.
- `shims/bcryptprimitives/` reports services.exe in session 0, as Windows does, so Go programs (Autodesk's CER service) know they run as a service.
- `shims/resprotocol/` wraps mshtml's res: protocol so `res:///...` URLs with no module (AutoCAD's ribbon images) map to the program's zone instead of failing; WPF throws "The URI specified is invalid" on the failure. Registered as the res protocol handler.
- `shims/tar/` a tar.exe that extracts zip archives (`tar -xf x.zip -C dir`), as Windows 10's does; Inventor's installer unpacks its content with it. Uses [miniz](https://github.com/richgel999/miniz) (MIT, `shims/tar/miniz-LICENSE`).
- `shims/sma/` a System.Management.Automation for Wine's .NET that runs a hosted PowerShell script with PowerShell 7 (pwsh.exe), so PS2EXE-wrapped scripts (Autodesk Revit's installer steps) run. Built with the .NET SDK.
- `tests/` `rangesrv.py` (a range-capable HTTP server) and `bitsclient.c` (a BITS client) to check qmgr under Wine
- `reg/` registry files applied to the prefix before the installer runs

## License

GPL v3, same as Mira. See `LICENSE`.
