# mira-winapp-shims

Clean-room shim DLLs and registry settings that let Windows-only apps (Microsoft 365 first, Adobe Creative Cloud and Autodesk later. Other apps may be added as needed) install and run on stock GE-Proton. Used by [Mira](https://github.com/Mira-Launcher/Mira).

## Layout

- `shims/` shim DLL sources, built with MinGW (`make`)
- `shims/qmgr/` a BITS service that downloads over parallel keep-alive HTTP connections and takes file ranges (Wine's cannot, so Office's Click-to-Run falls back to a slow transport). Written from the public BITS interfaces.
- `tests/` `rangesrv.py` (a range-capable HTTP server) and `bitsclient.c` (a BITS client) to check qmgr under Wine
- `reg/` registry files applied to the prefix before the installer runs

## License

GPL v3, same as Mira. See `LICENSE`.
