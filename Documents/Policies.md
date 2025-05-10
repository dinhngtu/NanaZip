# NanaZip Policies

NanaZip supports setting system-wide policies via creating Registry values in the key `HKLM\Software\NanaZip\Policies`.

Following is the list of currently-supported policy options.

## Supported policy values

### Propagate Zone.Id stream

This value controls Mark-of-the-Web (MOTW) propagation of archive files and overrides user settings.

- Name: `WriteZoneIdExtract`
- Type: REG_DWORD
- Value:
    - `0`: No
    - `1`: Yes (all files)
    - `2`: Only for unsafe extensions (does not support nested archives)
