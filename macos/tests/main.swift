// Unit tests for scrim:// URL parsing. Run with macos/test.sh.
import Foundation
var failures = 0
@MainActor func check(_ s: String, _ want: URLCommand?) {
    let got = try? URLCommand.parse(URL(string: s)!)
    let ok = got == want
    if !ok { failures += 1 }
    print(ok ? "ok  " : "FAIL", s, "->", got.map { "\($0)" } ?? "error")
}
check("scrim://pause", .pause)
check("scrim://Resume", .resume)
check("SCRIM://toggle", .toggle)
check("scrim://next", .next)
check("scrim://effect/condensation", .effect("condensation"))
check("scrim://effect", nil)
check("scrim://effect/", nil)
check("scrim://param?fog=0.8&drops=0.3", .params([("fog", "0.8"), ("drops", "0.3")]))
check("scrim://param", nil)
check("scrim://dance", nil)
check("https://pause", nil)
print(URLCommand.number("on") == 1 && URLCommand.number("0.25") == 0.25 && URLCommand.number("x") == nil ? "ok   number()" : "FAIL number()")
exit(failures == 0 ? 0 : 1)
