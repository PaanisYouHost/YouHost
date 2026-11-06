# YouHost 1.0.18

2026-11-05

## Fixes

- Unsolicited quit is logged and refused
- Plugin editors open on the message thread
- Scanner separates timeouts from crashes and retries once
- Plugin list and card open run off the message thread
- Buffer-size changes are not counted as dropouts
- Protected mode for plugins that crash or quit, 32 samples on that channel
- A protected crash mutes the channel, restarts the plugin, and fades audio back
- Plugin state is snapshotted off the audio thread
- Notices for crash, restored, not restored, and protected
- Session format stays 7, with an optional protect field
