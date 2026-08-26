# LCEDA Pro project

## Files

- `elec_piano.eprj2` is the latest editable LCEDA Pro project database copied on 2026-08-26 from the local project last modified on 2026-08-16.
- `archive/elec_piano_2026-08-08-21-36.epro2` is an older portable project snapshot retained as a recovery reference. It is not the authoritative latest design.

Open `elec_piano.eprj2` with LCEDA Pro. Make a local backup before allowing a newer application version to migrate the database format.

The checksums in `SHA256SUMS.txt` allow an exact integrity check after download. In PowerShell:

```powershell
Get-FileHash .\elec_piano.eprj2 -Algorithm SHA256
Get-FileHash .\archive\elec_piano_2026-08-08-21-36.epro2 -Algorithm SHA256
```

## Manufacturing warning

This is a design source release, not a promise that every board house rule set or component substitution is safe. Review the project and generate fresh Gerber, drill, BOM, and pick-and-place outputs from the exact revision you intend to manufacture.

