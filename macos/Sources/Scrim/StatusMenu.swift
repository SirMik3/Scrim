import AppKit
import ServiceManagement
import UniformTypeIdentifiers

/// The menu-bar menu. Rebuilt every time it opens, so it always reflects current state.
@MainActor
final class StatusMenu: NSObject, NSMenuDelegate {
    private let controller: DesktopController
    private let settings: Settings
    private let statusItem = NSStatusBar.system.statusItem(withLength: NSStatusItem.squareLength)
    private let menu = NSMenu()

    init(controller: DesktopController, settings: Settings) {
        self.controller = controller
        self.settings = settings
        super.init()
        statusItem.button?.image = NSImage(systemSymbolName: "cloud.rain", accessibilityDescription: "Scrim")
        menu.delegate = self
        menu.autoenablesItems = false
        statusItem.menu = menu
    }

    func menuNeedsUpdate(_ menu: NSMenu) {
        menu.removeAllItems()

        if let error = controller.lastError { menu.addItem(info("⚠︎ \(error)")) }

        menu.addItem(action(settings.userPaused ? "Resume" : "Pause", key: "p") { [unowned self] in
            settings.userPaused.toggle()
            controller.updatePause()
        })
        menu.addItem(.separator())

        // Effect picker
        let effectsItem = NSMenuItem(title: "Effect", action: nil, keyEquivalent: "")
        let effectsMenu = NSMenu()
        for effect in controller.effects {
            let item = action(effect.name) { [unowned self] in controller.selectEffect(effect) }
            item.state = controller.current?.effect.id == effect.id ? .on : .off
            effectsMenu.addItem(item)
        }
        if let m = controller.current?.effect.manifest, m.author != nil || m.license != nil {
            effectsMenu.addItem(.separator())
            if let author = m.author { effectsMenu.addItem(info("By \(author)")) }
            if let license = m.license { effectsMenu.addItem(info("License: \(license)")) }
            if let source = m.source, let url = URL(string: source) {
                effectsMenu.addItem(action("View Original…") { NSWorkspace.shared.open(url) })
            }
        }
        effectsMenu.addItem(.separator())
        effectsMenu.addItem(action("Reload Effects") { [unowned self] in controller.reloadEffects() })
        effectsMenu.addItem(action("Open Effects Folder") {
            let dir = EffectLibrary.userEffectsDirectory
            try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
            NSWorkspace.shared.open(dir)
        })
        effectsItem.submenu = effectsMenu
        menu.addItem(effectsItem)

        // Parameters of the current effect, generated from its manifest
        if let effect = controller.current?.effect, !effect.manifest.params.isEmpty {
            menu.addItem(.separator())
            for param in effect.manifest.params {
                let value = settings.paramValue(effect: effect, param: param)
                switch param.type {
                case .float:
                    let item = NSMenuItem()
                    item.view = SliderMenuView(title: param.label, value: value, range: param.range) { [unowned self] v in
                        settings.setParamValue(v, effect: effect, param: param)
                        controller.applyParams()
                    }
                    menu.addItem(item)
                case .bool:
                    let item = action(param.label) { [unowned self] in
                        settings.setParamValue(value > 0.5 ? 0 : 1, effect: effect, param: param)
                        controller.applyParams()
                    }
                    item.state = value > 0.5 ? .on : .off
                    menu.addItem(item)
                }
            }
            menu.addItem(action("Reset to Defaults") { [unowned self] in
                settings.resetParams(effect: effect)
                controller.applyParams()
            })
        }
        menu.addItem(.separator())

        // Wallpaper source
        if settings.customWallpaperPath == nil && controller.wallpaperUnreadable {
            menu.addItem(info("Wallpaper can't be read (Aerial/dynamic?)"))
        }
        menu.addItem(action("Choose Background Image…") { [unowned self] in chooseImage() })
        let systemItem = action("Use System Wallpaper") { [unowned self] in
            settings.customWallpaperPath = nil
            controller.refreshWallpapers(force: true)
        }
        systemItem.state = settings.customWallpaperPath == nil ? .on : .off
        menu.addItem(systemItem)
        menu.addItem(.separator())

        // Performance
        let fpsItem = NSMenuItem(title: "Frame Rate", action: nil, keyEquivalent: "")
        let fpsMenu = NSMenu()
        for fps in [20, 30, 60] {
            let item = action("\(fps) fps") { [unowned self] in
                settings.framesPerSecond = fps
                controller.applyRenderSettings()
            }
            item.state = settings.framesPerSecond == fps ? .on : .off
            fpsMenu.addItem(item)
        }
        fpsItem.submenu = fpsMenu
        menu.addItem(fpsItem)
        menu.addItem(toggle("Retina Resolution", settings.highQuality) { [unowned self] in
            settings.highQuality.toggle()
            controller.applyRenderSettings()
        })
        menu.addItem(toggle("Pause on Battery", settings.pauseOnBattery) { [unowned self] in
            settings.pauseOnBattery.toggle()
            controller.updatePause()
        })
        menu.addItem(toggle("Launch at Login", SMAppService.mainApp.status == .enabled) {
            let service = SMAppService.mainApp
            do {
                if service.status == .enabled { try service.unregister() } else { try service.register() }
            } catch {
                NSLog("Scrim: launch at login failed: %@", error.localizedDescription)
            }
        })
        menu.addItem(.separator())
        menu.addItem(action("Quit Scrim", key: "q") { NSApp.terminate(nil) })
    }

    private func chooseImage() {
        let panel = NSOpenPanel()
        panel.allowedContentTypes = [.image]
        panel.allowsMultipleSelection = false
        panel.message = "Choose an image to show behind the rain"
        NSApp.activate()
        guard panel.runModal() == .OK, let url = panel.url else { return }
        settings.customWallpaperPath = url.path
        controller.refreshWallpapers(force: true)
    }

    // MARK: Item helpers

    private func info(_ title: String) -> NSMenuItem {
        let item = NSMenuItem(title: title, action: nil, keyEquivalent: "")
        item.isEnabled = false
        return item
    }

    private func toggle(_ title: String, _ on: Bool, _ handler: @escaping @MainActor () -> Void) -> NSMenuItem {
        let item = action(title, handler)
        item.state = on ? .on : .off
        return item
    }

    private func action(_ title: String, key: String = "", _ handler: @escaping @MainActor () -> Void) -> NSMenuItem {
        ClosureMenuItem(title: title, key: key, handler: handler)
    }
}

@MainActor
private final class ClosureMenuItem: NSMenuItem {
    private let handler: @MainActor () -> Void

    init(title: String, key: String, handler: @escaping @MainActor () -> Void) {
        self.handler = handler
        super.init(title: title, action: #selector(fire), keyEquivalent: key)
        target = self
    }

    required init(coder: NSCoder) { fatalError("unused") }

    @objc private func fire() { handler() }
}

/// A labelled slider that lives inside a menu.
private final class SliderMenuView: NSView {
    private let valueLabel = NSTextField(labelWithString: "")
    private let onChange: @MainActor (Double) -> Void

    init(title: String, value: Double, range: ClosedRange<Double>, onChange: @escaping @MainActor (Double) -> Void) {
        self.onChange = onChange
        super.init(frame: NSRect(x: 0, y: 0, width: 260, height: 46))

        let titleLabel = NSTextField(labelWithString: title)
        titleLabel.font = .menuFont(ofSize: 0)
        titleLabel.frame = NSRect(x: 20, y: 24, width: 170, height: 17)
        addSubview(titleLabel)

        valueLabel.font = .monospacedDigitSystemFont(ofSize: NSFont.smallSystemFontSize, weight: .regular)
        valueLabel.textColor = .secondaryLabelColor
        valueLabel.alignment = .right
        valueLabel.frame = NSRect(x: 190, y: 24, width: 54, height: 17)
        addSubview(valueLabel)

        let slider = NSSlider(value: value, minValue: range.lowerBound, maxValue: range.upperBound,
                              target: self, action: #selector(changed(_:)))
        slider.isContinuous = true
        slider.controlSize = .small
        slider.frame = NSRect(x: 18, y: 4, width: 228, height: 18)
        addSubview(slider)
        showValue(value)
    }

    required init?(coder: NSCoder) { fatalError("unused") }

    private func showValue(_ v: Double) { valueLabel.stringValue = String(format: "%.2f", v) }

    @objc private func changed(_ sender: NSSlider) {
        showValue(sender.doubleValue)
        onChange(sender.doubleValue)
    }
}
