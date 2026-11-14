import AppKit
import IOKit.ps

/// Tracks system conditions under which rendering should stop entirely.
/// Per-window occlusion is handled by DesktopController; this covers global state.
@MainActor
final class PowerPolicy {
    private(set) var screensAsleep = false
    private(set) var sessionInactive = false
    private(set) var screenLocked = false
    private(set) var lowPowerMode = ProcessInfo.processInfo.isLowPowerModeEnabled
    /// The Mac is hot enough that macOS asks apps to cut back (serious or critical).
    private(set) var thermalPressure = PowerPolicy.readThermalPressure()
    private(set) var onBattery = PowerPolicy.readOnBattery()

    var onChange: (() -> Void)?
    private var tokens: [NSObjectProtocol] = []
    private var runLoopSource: CFRunLoopSource?
    private static let powerSourceChanged = Notification.Name("ScrimPowerSourceChanged")

    func shouldPause(pauseOnBattery: Bool) -> Bool {
        screensAsleep || sessionInactive || screenLocked || lowPowerMode || thermalPressure
            || (pauseOnBattery && onBattery)
    }

    func start() {
        let ws = NSWorkspace.shared.notificationCenter
        observe(ws, NSWorkspace.screensDidSleepNotification) { $0.screensAsleep = true }
        observe(ws, NSWorkspace.screensDidWakeNotification) { $0.screensAsleep = false }
        observe(ws, NSWorkspace.sessionDidResignActiveNotification) { $0.sessionInactive = true }
        observe(ws, NSWorkspace.sessionDidBecomeActiveNotification) { $0.sessionInactive = false }

        let dnc = DistributedNotificationCenter.default()
        observe(dnc, Notification.Name("com.apple.screenIsLocked")) { $0.screenLocked = true }
        observe(dnc, Notification.Name("com.apple.screenIsUnlocked")) { $0.screenLocked = false }

        observe(NotificationCenter.default, .NSProcessInfoPowerStateDidChange) {
            $0.lowPowerMode = ProcessInfo.processInfo.isLowPowerModeEnabled
        }
        observe(NotificationCenter.default, ProcessInfo.thermalStateDidChangeNotification) {
            $0.thermalPressure = Self.readThermalPressure()
        }
        observe(NotificationCenter.default, Self.powerSourceChanged) { $0.onBattery = Self.readOnBattery() }

        // IOKit calls this C callback on the main run loop when the power source changes.
        if let src = IOPSNotificationCreateRunLoopSource({ _ in
            NotificationCenter.default.post(name: PowerPolicy.powerSourceChanged, object: nil)
        }, nil)?.takeRetainedValue() {
            CFRunLoopAddSource(CFRunLoopGetMain(), src, .defaultMode)
            runLoopSource = src
        }
    }

    private func observe(_ center: NotificationCenter, _ name: Notification.Name,
                         _ update: @escaping @MainActor (PowerPolicy) -> Void) {
        tokens.append(center.addObserver(forName: name, object: nil, queue: .main) { [weak self] _ in
            MainActor.assumeIsolated {
                guard let self else { return }
                update(self)
                self.onChange?()
            }
        })
    }

    private static func readThermalPressure() -> Bool {
        switch ProcessInfo.processInfo.thermalState {
        case .serious, .critical: return true
        default: return false
        }
    }

    private static func readOnBattery() -> Bool {
        guard let info = IOPSCopyPowerSourcesInfo()?.takeRetainedValue(),
              let type = IOPSGetProvidingPowerSourceType(info)?.takeUnretainedValue() as String?
        else { return false }
        return type == kIOPMBatteryPowerKey
    }
}
