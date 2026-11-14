import Foundation

/// A `scrim://` URL, the macOS counterpart of Linux's `scrim msg`. Works from
/// `open scrim://toggle`, the Shortcuts app ("Open URL"), Raycast, Alfred, etc.
///
///   scrim://pause   scrim://resume   scrim://toggle   scrim://next
///   scrim://effect/condensation
///   scrim://param?fog=0.8&drops=0.3     (parameters of the current effect)
///
/// Unlike `scrim msg`, changes are saved, just like choosing them from the menu.
enum URLCommand: Equatable {
    case pause, resume, toggle, next
    case effect(String)
    case params([(String, String)])

    static func == (a: URLCommand, b: URLCommand) -> Bool {
        switch (a, b) {
        case (.pause, .pause), (.resume, .resume), (.toggle, .toggle), (.next, .next): return true
        case let (.effect(x), .effect(y)): return x == y
        case let (.params(x), .params(y)): return x.map { $0.0 + "=" + $0.1 } == y.map { $0.0 + "=" + $0.1 }
        default: return false
        }
    }

    enum ParseError: Error, CustomStringConvertible {
        case notScrim, unknown(String), missingEffect, noParams
        var description: String {
            switch self {
            case .notScrim: return "not a scrim:// URL"
            case .unknown(let c): return "unknown command '\(c)' (pause, resume, toggle, next, effect/ID, param?ID=VALUE)"
            case .missingEffect: return "scrim://effect needs an effect id, e.g. scrim://effect/rain"
            case .noParams: return "scrim://param needs ID=VALUE pairs, e.g. scrim://param?fog=0.8"
            }
        }
    }

    static func parse(_ url: URL) throws -> URLCommand {
        guard url.scheme?.lowercased() == "scrim" else { throw ParseError.notScrim }
        let command = (url.host ?? "").lowercased()
        switch command {
        case "pause": return .pause
        case "resume": return .resume
        case "toggle": return .toggle
        case "next": return .next
        case "effect":
            guard let id = url.pathComponents.dropFirst().first, !id.isEmpty else { throw ParseError.missingEffect }
            return .effect(id)
        case "param", "params":
            let items = URLComponents(url: url, resolvingAgainstBaseURL: false)?.queryItems ?? []
            let pairs = items.compactMap { item in item.value.map { (item.name, $0) } }
            guard !pairs.isEmpty else { throw ParseError.noParams }
            return .params(pairs)
        default:
            throw ParseError.unknown(command)
        }
    }

    /// "0.8" or "true"/"false"/"on"/"off" (for toggles).
    static func number(_ text: String) -> Double? {
        switch text.lowercased() {
        case "true", "on", "yes": return 1
        case "false", "off", "no": return 0
        default: return Double(text)
        }
    }
}
