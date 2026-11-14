import AppKit
import ServiceManagement

@MainActor
final class AppDelegate: NSObject, NSApplicationDelegate {
    private var controller: DesktopController?
    private var statusMenu: StatusMenu?
    private let power = PowerPolicy()
    private let settings = Settings()
    /// scrim:// URLs that arrive while launching, before the controller exists.
    private var pendingURLs: [URL] = []

    func applicationDidFinishLaunching(_ notification: Notification) {
        do {
            let gpu = try GPU()
            let controller = DesktopController(gpu: gpu, settings: settings, power: power)
            power.start()
            controller.start()
            statusMenu = StatusMenu(controller: controller, settings: settings)
            self.controller = controller
            pendingURLs.forEach(handle)
            pendingURLs = []
        } catch {
            let alert = NSAlert()
            alert.messageText = "Scrim can't start"
            alert.informativeText = error.localizedDescription
            alert.runModal()
            NSApp.terminate(nil)
        }
    }

    func application(_ application: NSApplication, open urls: [URL]) {
        if controller == nil { pendingURLs += urls } else { urls.forEach(handle) }
    }

    private func handle(_ url: URL) {
        guard let controller else { return }
        do {
            switch try URLCommand.parse(url) {
            case .pause: settings.userPaused = true; controller.updatePause()
            case .resume: settings.userPaused = false; controller.updatePause()
            case .toggle: settings.userPaused.toggle(); controller.updatePause()
            case .next: controller.selectNextEffect()
            case .effect(let id):
                guard let effect = controller.effects.first(where: { $0.id == id }) else {
                    NSLog("Scrim: %@: no effect '%@'", url.absoluteString, id)
                    return
                }
                controller.selectEffect(effect)
            case .params(let pairs):
                guard let effect = controller.current?.effect else { return }
                for (id, text) in pairs {
                    guard let param = effect.manifest.params.first(where: { $0.id == id }),
                          let value = URLCommand.number(text) else {
                        NSLog("Scrim: %@: effect '%@' has no parameter %@=%@", url.absoluteString, effect.id, id, text)
                        continue
                    }
                    settings.setParamValue(value, effect: effect, param: param)
                }
                controller.applyParams()
            }
        } catch {
            NSLog("Scrim: %@: %@", url.absoluteString, String(describing: error))
        }
    }
}

if CommandLine.arguments.contains("--snapshot") {
    exit(Snapshot.run(CommandLine.arguments))
}

// Scrim --login-item status|on|off — the same switch as the menu's "Launch at Login".
if let i = CommandLine.arguments.firstIndex(of: "--login-item") {
    let service = SMAppService.mainApp
    let arg = i + 1 < CommandLine.arguments.count ? CommandLine.arguments[i + 1] : "status"
    do {
        switch arg {
        case "on": try service.register()
        case "off": try service.unregister()
        default: break
        }
        let names: [SMAppService.Status: String] = [.notRegistered: "not registered", .enabled: "enabled",
                                                    .requiresApproval: "requires approval", .notFound: "not found"]
        print("launch at login: \(names[service.status] ?? "unknown")")
        exit(0)
    } catch {
        FileHandle.standardError.write(Data("launch at login: \(error.localizedDescription)\n".utf8))
        exit(1)
    }
}

let app = NSApplication.shared
let delegate = AppDelegate()
app.delegate = delegate
app.setActivationPolicy(.accessory)   // menu-bar only, no Dock icon (also LSUIElement in Info.plist)
app.run()
