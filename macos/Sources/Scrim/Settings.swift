import Foundation

/// User preferences, persisted in UserDefaults.
@MainActor
final class Settings {
    private let defaults = UserDefaults.standard

    var effectID: String {
        get { defaults.string(forKey: "effectID") ?? "rain" }
        set { defaults.set(newValue, forKey: "effectID") }
    }

    var userPaused: Bool {
        get { defaults.bool(forKey: "userPaused") }
        set { defaults.set(newValue, forKey: "userPaused") }
    }

    var framesPerSecond: Int {
        get { let v = defaults.integer(forKey: "fps"); return v > 0 ? v : 30 }
        set { defaults.set(newValue, forKey: "fps") }
    }

    /// Render at full Retina resolution instead of one pixel per point.
    var highQuality: Bool {
        get { defaults.bool(forKey: "highQuality") }
        set { defaults.set(newValue, forKey: "highQuality") }
    }

    var pauseOnBattery: Bool {
        get { defaults.object(forKey: "pauseOnBattery") as? Bool ?? true }
        set { defaults.set(newValue, forKey: "pauseOnBattery") }
    }

    /// When set, used instead of the system wallpaper on every screen.
    var customWallpaperPath: String? {
        get { defaults.string(forKey: "customWallpaperPath") }
        set { defaults.set(newValue, forKey: "customWallpaperPath") }
    }

    func paramValue(effect: Effect, param: EffectManifest.Param) -> Double {
        let stored = defaults.dictionary(forKey: "params.\(effect.id)")?[param.id] as? Double
        let value = stored ?? param.defaultValue
        return min(max(value, param.range.lowerBound), param.range.upperBound)
    }

    func setParamValue(_ value: Double, effect: Effect, param: EffectManifest.Param) {
        var dict = defaults.dictionary(forKey: "params.\(effect.id)") ?? [:]
        dict[param.id] = value
        defaults.set(dict, forKey: "params.\(effect.id)")
    }

    func resetParams(effect: Effect) {
        defaults.removeObject(forKey: "params.\(effect.id)")
    }
}
