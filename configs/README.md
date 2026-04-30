# configs/

This directory contains JSON experiment configuration files.

Active files:

- `experiment.json` — canonical experiment configuration used by the tools.
- `experiment.detected.json` — auto-generated detection output (capability probe).

Backups:

- `backups/` — legacy or timestamped backups are stored here (do not edit in-place).

Guidance:

- Edit `experiment.json` for your live runs. Keep `receiver.use_vendor_plugins` set to `false` unless you intentionally enable vendor plugin loading.
- If you need to restore a previous config, copy the desired file from `backups/` into `experiment.json`.
