# Suppress Wake Notification Spam

A Windhawk mod for Windows 11 that prevents queued notifications from flooding the desktop with banners and sounds when the monitor wakes, the session unlocks, or the PC resumes.

Windows can hold notifications while the user is away and then replay their banners when the system becomes active again. This mod temporarily enables Windows' own Do Not Disturb profile while the display is off, the session is locked, or the system is suspended. After wake/unlock, it waits briefly and restores the user's previous DND state.

Notifications are **not deleted**. They remain available in Notification Center.

## Behavior

- Display off, session lock, or suspend: temporarily enable DND.
- Display on, unlock, or resume: keep DND active for a short configurable delay, then restore the previous state.
- If DND was already enabled by the user, the mod leaves it enabled.
- If the DND profile changes externally while the mod is active, the mod does not overwrite the newer choice.
- A recovery marker helps restore the previous DND state if the Windhawk helper is terminated while it owns the temporary change.

## Privacy

The mod does not inspect or record notification contents. It does not read notification text, senders, subjects, message bodies, or application payloads, and it does not hook individual notification-producing applications.

Windhawk debug logs from this mod contain only operational information such as display/session state, DND profile state, restore timing, and API/error information.

## Settings

- **Restore delay after wake/unlock:** default 2500 ms.
- **Temporary suppression profile:** Priority only by default; Alarms only is available as a stricter option.
- Suppression can independently be enabled or disabled for display-off, Windows lock, and suspend states.

## Installation

Until the mod is available in the Windhawk catalog:

1. Install [Windhawk](https://windhawk.net/).
2. In Windhawk, create a new local mod.
3. Replace the generated source with [suppress-wake-notification-spam.wh.cpp](./suppress-wake-notification-spam.wh.cpp).
4. Compile and enable the mod.

The default settings are the configuration used during the initial real-world testing.

## Implementation

The mod runs as a Windhawk tool mod rather than injecting persistent background logic into Explorer. It uses:

- GUID_SESSION_DISPLAY_STATUS for display-power state;
- WTS session notifications for lock/unlock state;
- the Windows QuietHoursSettings COM service to read and set the current DND profile.

The Quiet Hours interface is undocumented. The interface definition and IDs are based on the reverse-engineered MIT-licensed IDL used by projects such as Telegram Desktop and other Windows utilities.

## Compatibility

Designed for Windows 11. Because the DND control interface is undocumented, a future Windows update could require an adjustment.

## License

MIT. See [LICENSE](./LICENSE).
