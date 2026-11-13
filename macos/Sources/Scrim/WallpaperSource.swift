import AppKit
import ImageIO
import Metal

/// Loads the wallpaper for a screen as a texture no larger than the render size.
/// Reads the image file directly; no screen capture (and no Screen Recording permission).
@MainActor
enum WallpaperSource {
    struct Loaded {
        let texture: MTLTexture
        /// The file that was read, or nil when the fallback gradient is used.
        let url: URL?
        let modified: Date?
    }

    static func currentURL(for screen: NSScreen, customPath: String?) -> URL? {
        if let customPath { return URL(fileURLWithPath: customPath) }
        return NSWorkspace.shared.desktopImageURL(for: screen)
    }

    static func modificationDate(_ url: URL?) -> Date? {
        guard let url else { return nil }
        return (try? url.resourceValues(forKeys: [.contentModificationDateKey]))?.contentModificationDate
    }

    static func load(device: MTLDevice, screen: NSScreen, customPath: String?, maxPixels: Int) -> Loaded {
        load(device: device, url: currentURL(for: screen, customPath: customPath), maxPixels: maxPixels)
    }

    static func load(device: MTLDevice, url: URL?, maxPixels: Int) -> Loaded {
        if let url, let image = decode(url, maxPixels: maxPixels), let texture = upload(image, device: device) {
            return Loaded(texture: texture, url: url, modified: modificationDate(url))
        }
        return Loaded(texture: fallback(device: device), url: nil, modified: nil)
    }

    /// Redraws the image into a known layout (8-bit RGBA, sRGB) so that P3, 16-bit,
    /// greyscale or alpha-first sources all end up as the same texture format.
    private static func upload(_ image: CGImage, device: MTLDevice) -> MTLTexture? {
        let w = image.width, h = image.height, bpr = w * 4
        guard w > 0, h > 0,
              let ctx = CGContext(data: nil, width: w, height: h, bitsPerComponent: 8, bytesPerRow: bpr,
                                  space: CGColorSpace(name: CGColorSpace.sRGB)!,
                                  bitmapInfo: CGImageAlphaInfo.noneSkipLast.rawValue),
              let data = ctx.data else { return nil }
        ctx.interpolationQuality = .none
        ctx.draw(image, in: CGRect(x: 0, y: 0, width: w, height: h))

        let td = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .rgba8Unorm, width: w, height: h, mipmapped: true)
        td.usage = .shaderRead
        td.storageMode = device.hasUnifiedMemory ? .shared : .managed
        guard let texture = device.makeTexture(descriptor: td) else { return nil }
        // CGContext rows start at the top of the image, matching the top-left uv origin.
        texture.replace(region: MTLRegionMake2D(0, 0, w, h), mipmapLevel: 0, withBytes: data, bytesPerRow: bpr)

        // Full mip chain so effects can blur cheaply with textureLod (spec format 1).
        // Wallpaper loads are rare, so a short-lived queue is fine here.
        if let queue = device.makeCommandQueue(), let cmd = queue.makeCommandBuffer(),
           let blit = cmd.makeBlitCommandEncoder() {
            blit.generateMipmaps(for: texture)
            blit.endEncoding()
            cmd.commit()
            cmd.waitUntilCompleted()
        }
        return texture
    }

    /// Decodes straight to a downscaled image so a 6K HEIC never sits in memory at full size.
    private static func decode(_ url: URL, maxPixels: Int) -> CGImage? {
        var isDir: ObjCBool = false
        guard FileManager.default.fileExists(atPath: url.path, isDirectory: &isDir), !isDir.boolValue,
              let src = CGImageSourceCreateWithURL(url as CFURL, [kCGImageSourceShouldCache: false] as CFDictionary)
        else { return nil }
        let opts: [CFString: Any] = [
            kCGImageSourceCreateThumbnailFromImageAlways: true,
            kCGImageSourceCreateThumbnailWithTransform: true,
            kCGImageSourceThumbnailMaxPixelSize: maxPixels,
            kCGImageSourceShouldCacheImmediately: true,
        ]
        return CGImageSourceCreateThumbnailAtIndex(src, 0, opts as CFDictionary)
    }

    /// A dark blue-grey gradient for wallpapers that can't be read (e.g. Aerials).
    private static func fallback(device: MTLDevice) -> MTLTexture {
        let w = 4, h = 64
        var pixels = [UInt8](repeating: 255, count: w * h * 4)
        for y in 0..<h {
            let t = Double(y) / Double(h - 1)
            let r = UInt8(28 + 20 * t), g = UInt8(36 + 22 * t), b = UInt8(52 + 18 * t)
            for x in 0..<w {
                let i = (y * w + x) * 4
                pixels[i] = b; pixels[i + 1] = g; pixels[i + 2] = r   // BGRA
            }
        }
        let td = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .bgra8Unorm, width: w, height: h, mipmapped: false)
        td.usage = .shaderRead
        let tex = device.makeTexture(descriptor: td)!
        tex.replace(region: MTLRegionMake2D(0, 0, w, h), mipmapLevel: 0, withBytes: pixels, bytesPerRow: w * 4)
        return tex
    }
}
