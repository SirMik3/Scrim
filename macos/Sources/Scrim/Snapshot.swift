import AppKit
import ImageIO
import Metal
import UniformTypeIdentifiers

/// Renders one frame of an effect offscreen to a PNG — for effect authoring and testing.
///
///   Scrim --snapshot out.png [--effect rain] [--time 12.5] [--size 1728x1117]
///             [--wallpaper image.jpg] [--param id=value ...]
@MainActor
enum Snapshot {
    static func run(_ args: [String]) -> Int32 {
        func value(_ flag: String) -> String? {
            guard let i = args.firstIndex(of: flag), i + 1 < args.count else { return nil }
            return args[i + 1]
        }
        guard let out = value("--snapshot") else { return fail("--snapshot needs an output path") }
        let effectID = value("--effect") ?? "rain"
        let time = Double(value("--time") ?? "") ?? 10

        var size = NSScreen.main?.frame.size ?? CGSize(width: 1728, height: 1117)
        if let s = value("--size") {
            let parts = s.split(separator: "x").compactMap { Double($0) }
            guard parts.count == 2 else { return fail("--size must look like 1728x1117") }
            size = CGSize(width: parts[0], height: parts[1])
        }

        var overrides: [String: Double] = [:]
        for (i, a) in args.enumerated() where a == "--param" && i + 1 < args.count {
            let kv = args[i + 1].split(separator: "=", maxSplits: 1)
            if kv.count == 2, let v = Double(kv[1]) { overrides[String(kv[0])] = v }
        }

        do {
            let gpu = try GPU()
            guard let effect = EffectLibrary.loadAll().first(where: { $0.id == effectID }) else {
                return fail("effect '\(effectID)' not found in \(EffectLibrary.searchDirectories.map(\.path))")
            }
            let compiled = try gpu.compiled(effect)

            let maxPixels = Int(max(size.width, size.height))
            let wallpaper: WallpaperSource.Loaded
            if let path = value("--wallpaper") {
                wallpaper = WallpaperSource.load(device: gpu.device, url: URL(fileURLWithPath: path), maxPixels: maxPixels)
            } else if let screen = NSScreen.main {
                wallpaper = WallpaperSource.load(device: gpu.device, screen: screen, customPath: nil, maxPixels: maxPixels)
            } else {
                wallpaper = WallpaperSource.load(device: gpu.device, url: nil, maxPixels: maxPixels)
            }
            if wallpaper.url == nil { print("note: wallpaper unreadable, using fallback gradient") }

            let renderer = EffectRenderer(gpu: gpu, compiled: compiled, wallpaper: wallpaper.texture)
            renderer.setParams(effect.manifest.params.map {
                Float(overrides[$0.id] ?? $0.defaultValue)
            })
            renderer.setTime(time)

            let td = MTLTextureDescriptor.texture2DDescriptor(
                pixelFormat: GPU.pixelFormat, width: Int(size.width), height: Int(size.height), mipmapped: false)
            td.usage = [.renderTarget, .shaderRead]
            td.storageMode = .managed
            guard let target = gpu.device.makeTexture(descriptor: td),
                  let cmd = gpu.queue.makeCommandBuffer() else { return fail("could not allocate GPU resources") }

            renderer.encode(into: cmd, finalTarget: target, delta: 1.0 / 30)
            if let blit = cmd.makeBlitCommandEncoder() {
                blit.synchronize(resource: target)
                blit.endEncoding()
            }
            let start = CACurrentMediaTime()
            cmd.commit()
            cmd.waitUntilCompleted()
            if let error = cmd.error { return fail("GPU error: \(error.localizedDescription)") }
            let gpuMs = (cmd.gpuEndTime - cmd.gpuStartTime) * 1000
            let wallMs = (CACurrentMediaTime() - start) * 1000

            try writePNG(target, to: URL(fileURLWithPath: out))
            print(String(format: "wrote %@ (%dx%d, gpu %.2f ms, wall %.2f ms, first frame incl. setup)",
                         out, target.width, target.height, gpuMs, wallMs))

            // Steady-state cost: render more frames and report the median GPU time and memory.
            var times: [Double] = []
            for i in 0..<60 {
                guard let c = gpu.queue.makeCommandBuffer() else { break }
                renderer.setTime(time + Double(i) / 30)
                renderer.encode(into: c, finalTarget: target, delta: 1.0 / 30)
                c.commit()
                c.waitUntilCompleted()
                times.append((c.gpuEndTime - c.gpuStartTime) * 1000)
            }
            times.sort()
            print(String(format: "steady state: median gpu %.2f ms/frame over %d frames, footprint %.1f MB",
                         times.isEmpty ? 0 : times[times.count / 2], times.count, physicalFootprintMB()))
            return 0
        } catch {
            return fail(error.localizedDescription)
        }
    }

    private static func writePNG(_ texture: MTLTexture, to url: URL) throws {
        let w = texture.width, h = texture.height, bpr = w * 4
        var bytes = [UInt8](repeating: 0, count: bpr * h)
        texture.getBytes(&bytes, bytesPerRow: bpr, from: MTLRegionMake2D(0, 0, w, h), mipmapLevel: 0)
        let info = CGBitmapInfo.byteOrder32Little.rawValue | CGImageAlphaInfo.noneSkipFirst.rawValue
        guard let ctx = CGContext(data: &bytes, width: w, height: h, bitsPerComponent: 8, bytesPerRow: bpr,
                                  space: CGColorSpace(name: CGColorSpace.sRGB)!, bitmapInfo: info),
              let image = ctx.makeImage(),
              let dest = CGImageDestinationCreateWithURL(url as CFURL, UTType.png.identifier as CFString, 1, nil)
        else { throw EffectError(message: "could not encode PNG") }
        CGImageDestinationAddImage(dest, image, nil)
        guard CGImageDestinationFinalize(dest) else { throw EffectError(message: "could not write \(url.path)") }
    }

    private static func fail(_ message: String) -> Int32 {
        FileHandle.standardError.write(Data("snapshot: \(message)\n".utf8))
        return 1
    }
}
