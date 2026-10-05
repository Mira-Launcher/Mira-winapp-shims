# mira-winapp-shims

Clean-room shim DLLs and registry settings that let Windows-only apps (Microsoft 365 first, Adobe Creative Cloud and Autodesk later. Other apps may be added as needed) install and run on stock GE-Proton. Used by [Mira](https://github.com/Mira-Launcher/Mira).

## Layout

- `shims/` shim DLL sources, built with MinGW (`make`)
- `reg/` registry files applied to the prefix before the installer runs
- `NOTES.md` which app build and Proton build each fix was tested on

## License

GPL v3, same as Mira. See `LICENSE`.
