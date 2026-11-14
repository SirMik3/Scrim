import Metal

/// Device-wide Metal state shared by every screen.
@MainActor
final class GPU {
    static let pixelFormat: MTLPixelFormat = .bgra8Unorm

    let device: MTLDevice
    let queue: MTLCommandQueue
    let sampler: MTLSamplerState
    /// Bound as `uFeedback` for passes that aren't persistent.
    let blackTexture: MTLTexture
    private let vertexFunction: MTLFunction
    private var cache: [String: CompiledEffect] = [:]

    /// Host side of the effect contract: one full-screen triangle that provides
    /// `vUV` at location 0, with (0,0) at the top-left and +y down.
    private static let vertexSource = """
    #include <metal_stdlib>
    using namespace metal;
    struct RPVertexOut {
        float4 position [[position]];
        float2 vUV [[user(locn0)]];
    };
    vertex RPVertexOut rp_fullscreen(uint vid [[vertex_id]]) {
        float2 p = float2((vid << 1) & 2, vid & 2);
        RPVertexOut o;
        o.position = float4(p * 2.0 - 1.0, 0.0, 1.0);
        o.vUV = float2(p.x, 1.0 - p.y);
        return o;
    }
    """

    init() throws {
        // Prefer the integrated (low-power) GPU on Macs that have two.
        let all = MTLCopyAllDevices()
        guard let device = all.first(where: { $0.isLowPower && !$0.isRemovable })
            ?? MTLCreateSystemDefaultDevice() else {
            throw EffectError(message: "No Metal device available")
        }
        self.device = device
        guard let queue = device.makeCommandQueue() else { throw EffectError(message: "No command queue") }
        self.queue = queue

        let sd = MTLSamplerDescriptor()
        sd.minFilter = .linear
        sd.magFilter = .linear
        sd.mipFilter = .linear          // trilinear on the mipmapped wallpaper (spec: textureLod)
        sd.sAddressMode = .clampToEdge
        sd.tAddressMode = .clampToEdge
        guard let sampler = device.makeSamplerState(descriptor: sd) else { throw EffectError(message: "No sampler") }
        self.sampler = sampler

        let bd = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: Self.pixelFormat, width: 1, height: 1, mipmapped: false)
        bd.usage = .shaderRead
        guard let black = device.makeTexture(descriptor: bd) else { throw EffectError(message: "No texture") }
        var zero: UInt32 = 0
        black.replace(region: MTLRegionMake2D(0, 0, 1, 1), mipmapLevel: 0, withBytes: &zero, bytesPerRow: 4)
        blackTexture = black

        let lib = try device.makeLibrary(source: Self.vertexSource, options: nil)
        guard let vfn = lib.makeFunction(name: "rp_fullscreen") else { throw EffectError(message: "No vertex function") }
        vertexFunction = vfn
    }

    /// The texture format a pass renders into. The last pass draws to the screen.
    static func targetFormat(_ effect: Effect, pass index: Int) -> MTLPixelFormat {
        let last = index == effect.manifest.passes.count - 1
        return !last && effect.manifest.passes[index].pixelFormat == "rgba16f" ? .rgba16Float : pixelFormat
    }

    func compiled(_ effect: Effect) throws -> CompiledEffect {
        if let hit = cache[effect.id] { return hit }
        let options = MTLCompileOptions()
        if #available(macOS 15.0, *) { options.mathMode = .fast } else { options.fastMathEnabled = true }
        var pipelines: [MTLRenderPipelineState] = []
        for i in effect.manifest.passes.indices {
            let source = try effect.metalSource(forPass: i)
            let lib: MTLLibrary
            do {
                lib = try device.makeLibrary(source: source, options: options)
            } catch {
                throw EffectError(message: "\(effect.manifest.passes[i].shader): \(error.localizedDescription)")
            }
            guard let fn = lib.makeFunction(name: "main0") else {
                throw EffectError(message: "\(effect.manifest.passes[i].shader): no main0 function")
            }
            let pd = MTLRenderPipelineDescriptor()
            pd.label = "\(effect.id).\(effect.manifest.passes[i].stem)"
            pd.vertexFunction = vertexFunction
            pd.fragmentFunction = fn
            pd.colorAttachments[0].pixelFormat = Self.targetFormat(effect, pass: i)
            pipelines.append(try device.makeRenderPipelineState(descriptor: pd))
        }
        let result = CompiledEffect(effect: effect, pipelines: pipelines)
        cache[effect.id] = result
        return result
    }

    func clearCache() { cache.removeAll() }
}

struct CompiledEffect {
    let effect: Effect
    let pipelines: [MTLRenderPipelineState]
}
