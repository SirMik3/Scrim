import Foundation

/// manifest.json, as defined in spec/effect-format.md (formats 1 and 2).
struct EffectManifest: Decodable {
    struct Pass: Decodable {
        let shader: String
        let scale: Double?
        /// Format 2: keeps its output between frames and reads it back as `uFeedback`.
        let persistent: Bool?
        /// Format 2: "rgba8" (default) or "rgba16f".
        let pixelFormat: String?

        var stem: String { (shader as NSString).deletingPathExtension }
    }

    struct Param: Decodable {
        enum Kind: String, Decodable { case float, bool }

        let id: String
        let label: String
        let type: Kind
        let min: Double?
        let max: Double?
        let defaultValue: Double

        enum CodingKeys: String, CodingKey { case id, label, type, min, max, defaultValue = "default" }

        init(from decoder: Decoder) throws {
            let c = try decoder.container(keyedBy: CodingKeys.self)
            id = try c.decode(String.self, forKey: .id)
            label = try c.decode(String.self, forKey: .label)
            type = try c.decode(Kind.self, forKey: .type)
            min = try c.decodeIfPresent(Double.self, forKey: .min)
            max = try c.decodeIfPresent(Double.self, forKey: .max)
            switch type {
            case .float: defaultValue = try c.decode(Double.self, forKey: .defaultValue)
            case .bool: defaultValue = try c.decode(Bool.self, forKey: .defaultValue) ? 1 : 0
            }
        }

        var range: ClosedRange<Double> {
            switch type {
            case .float: return (min ?? 0)...(max ?? 1)
            case .bool: return 0...1
            }
        }
    }

    let format: Int
    let id: String
    let name: String
    let version: String?
    let author: String?
    let license: String?
    let source: String?
    let passes: [Pass]
    let params: [Param]

    enum CodingKeys: String, CodingKey { case format, id, name, version, author, license, source, passes, params }

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        format = try c.decode(Int.self, forKey: .format)
        id = try c.decode(String.self, forKey: .id)
        name = try c.decode(String.self, forKey: .name)
        version = try c.decodeIfPresent(String.self, forKey: .version)
        author = try c.decodeIfPresent(String.self, forKey: .author)
        license = try c.decodeIfPresent(String.self, forKey: .license)
        source = try c.decodeIfPresent(String.self, forKey: .source)
        passes = try c.decode([Pass].self, forKey: .passes)
        params = try c.decodeIfPresent([Param].self, forKey: .params) ?? []
    }
}

struct EffectError: LocalizedError {
    let message: String
    var errorDescription: String? { message }
}

/// An effect folder on disk with a validated manifest.
struct Effect {
    static let maxPasses = 8
    static let maxParams = 16
    static let formats = [1, 2]
    static let pixelFormats = ["rgba8", "rgba16f"]

    let manifest: EffectManifest
    let directory: URL

    var id: String { manifest.id }
    var name: String { manifest.name }

    init(directory: URL) throws {
        let data = try Data(contentsOf: directory.appendingPathComponent("manifest.json"))
        let manifest = try JSONDecoder().decode(EffectManifest.self, from: data)
        guard Self.formats.contains(manifest.format) else {
            throw EffectError(message: "unsupported format \(manifest.format) (a newer Scrim may be needed)")
        }
        guard (1...Self.maxPasses).contains(manifest.passes.count) else {
            throw EffectError(message: "needs 1–\(Self.maxPasses) passes")
        }
        guard manifest.params.count <= Self.maxParams else {
            throw EffectError(message: "at most \(Self.maxParams) params")
        }
        for (i, pass) in manifest.passes.enumerated() {
            let last = i == manifest.passes.count - 1
            if pass.persistent != nil || pass.pixelFormat != nil {
                guard manifest.format >= 2 else {
                    throw EffectError(message: "pass \(i): \"persistent\" and \"pixelFormat\" need format 2")
                }
                guard !last else { throw EffectError(message: "the last pass can't be persistent or set pixelFormat") }
            }
            if let f = pass.pixelFormat, !Self.pixelFormats.contains(f) {
                throw EffectError(message: "pass \(i): pixelFormat must be rgba8 or rgba16f")
            }
            let metal = directory.appendingPathComponent("build/\(pass.stem).metal")
            guard FileManager.default.fileExists(atPath: metal.path) else {
                throw EffectError(message: "missing build/\(pass.stem).metal — run tools/effectc")
            }
        }
        self.manifest = manifest
        self.directory = directory
    }

    func metalSource(forPass index: Int) throws -> String {
        let stem = manifest.passes[index].stem
        return try String(contentsOf: directory.appendingPathComponent("build/\(stem).metal"), encoding: .utf8)
    }

    func isPersistent(_ index: Int) -> Bool { manifest.passes[index].persistent ?? false }
    var hasPersistentPasses: Bool { manifest.passes.indices.contains(where: isPersistent) }

    func scale(forPass index: Int) -> Double {
        min(max(manifest.passes[index].scale ?? 1, 0.125), 1)
    }
}

/// Finds effect folders in the app bundle and the user's Application Support folder.
enum EffectLibrary {
    static var userEffectsDirectory: URL {
        FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("Scrim/Effects", isDirectory: true)
    }

    static var searchDirectories: [URL] {
        var dirs: [URL] = []
        if let env = ProcessInfo.processInfo.environment["SCRIM_EFFECTS_DIR"] {
            dirs.append(URL(fileURLWithPath: env, isDirectory: true))
        }
        if let res = Bundle.main.resourceURL {
            dirs.append(res.appendingPathComponent("effects", isDirectory: true))
        }
        dirs.append(userEffectsDirectory)
        return dirs
    }

    /// Later directories override earlier ones with the same effect id.
    static func loadAll() -> [Effect] {
        var byID: [String: Effect] = [:]
        let fm = FileManager.default
        for dir in searchDirectories {
            guard let entries = try? fm.contentsOfDirectory(at: dir, includingPropertiesForKeys: nil) else { continue }
            for entry in entries where fm.fileExists(atPath: entry.appendingPathComponent("manifest.json").path) {
                do {
                    let effect = try Effect(directory: entry)
                    byID[effect.id] = effect
                } catch {
                    NSLog("Scrim: skipping effect at %@: %@", entry.path, error.localizedDescription)
                }
            }
        }
        return byID.values.sorted { $0.name.localizedCaseInsensitiveCompare($1.name) == .orderedAscending }
    }
}
