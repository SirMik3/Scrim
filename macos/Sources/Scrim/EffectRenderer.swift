import MetalKit
import QuartzCore

/// Runs an effect's passes into one screen's MTKView.
@MainActor
final class EffectRenderer: NSObject, MTKViewDelegate {
    private let gpu: GPU
    private var compiled: CompiledEffect
    private var wallpaper: MTLTexture
    private var params = [SIMD4<Float>](repeating: .zero, count: Effect.maxParams)
    /// Per pass: no textures for the last pass (it draws to the screen), one for an
    /// ordinary pass, two for a persistent pass (each step reads [cur], writes the other).
    private var targets: [[MTLTexture]] = []
    private var targetSize: CGSize = .zero
    private var cur = 0
    /// Persistent state: steps since it was cleared, and the time of the last step.
    private var steps: Double = 0
    private var lastStepTime: Double = 0
    private var needsClear: [MTLTexture] = []

    /// While true, draws show the current state without advancing time or persistent
    /// passes (the still frame left up when pausing).
    var holdState = false

    /// Rendered time; it only advances while frames are drawn.
    private var time: Double = 0
    private var frame: Double = 0
    /// Frames drawn since the last read; used by the SCRIM_DEBUG log.
    private(set) var framesSinceCheck = 0
    func takeFrameCount() -> Int { defer { framesSinceCheck = 0 }; return framesSinceCheck }
    private var lastTimestamp: CFTimeInterval?
    private static let timeWrap: Double = 20_000

    init(gpu: GPU, compiled: CompiledEffect, wallpaper: MTLTexture) {
        self.gpu = gpu
        self.compiled = compiled
        self.wallpaper = wallpaper
    }

    func setEffect(_ compiled: CompiledEffect) {
        self.compiled = compiled
        targets = []          // pass count/scales may differ
        targetSize = .zero
    }

    func setWallpaper(_ texture: MTLTexture) { wallpaper = texture }

    func setParams(_ values: [Float]) {
        for i in params.indices { params[i] = SIMD4(i < values.count ? values[i] : 0, 0, 0, 0) }
    }

    /// Call when the view resumes so paused time isn't counted as one long frame.
    func resetClock() { lastTimestamp = nil }

    func mtkView(_ view: MTKView, drawableSizeWillChange size: CGSize) {
        targets = []
        targetSize = .zero
    }

    private func ensureTargets(for size: CGSize) {
        let passCount = compiled.pipelines.count
        guard size != targetSize || targets.count != passCount else { return }
        targetSize = size
        // New targets mean new persistent state (the spec clears it on resize and effect change).
        steps = 0
        cur = 0
        needsClear = []
        let effect = compiled.effect
        targets = (0..<passCount).map { i in
            guard i < passCount - 1 else { return [] }   // last pass draws to the screen
            let s = effect.scale(forPass: i)
            let td = MTLTextureDescriptor.texture2DDescriptor(
                pixelFormat: GPU.targetFormat(effect, pass: i),
                width: max(1, Int((size.width * s).rounded())),
                height: max(1, Int((size.height * s).rounded())),
                mipmapped: false)
            td.usage = [.renderTarget, .shaderRead]
            td.storageMode = .private
            let count = effect.isPersistent(i) ? 2 : 1
            let textures = (0..<count).compactMap { _ in gpu.device.makeTexture(descriptor: td) }
            if effect.isPersistent(i) { needsClear += textures }
            return textures
        }
    }

    /// Jump to a given time (used by --snapshot).
    func setTime(_ t: Double) { time = t; lastTimestamp = nil }

    func draw(in view: MTKView) {
        let size = view.drawableSize
        guard size.width > 0, size.height > 0 else { return }

        let now = CACurrentMediaTime()
        let delta = lastTimestamp.map { min(max(now - $0, 0), 0.1) } ?? (1.0 / Double(max(view.preferredFramesPerSecond, 1)))
        lastTimestamp = now
        if !holdState {
            time += delta
            if time > Self.timeWrap { time = 0 }
        }

        guard let drawable = view.currentDrawable,
              let cmd = gpu.queue.makeCommandBuffer() else { return }
        encode(into: cmd, finalTarget: drawable.texture, delta: delta, step: !holdState)
        framesSinceCheck += 1
        cmd.present(drawable)
        cmd.commit()
    }

    /// Encodes every pass; the last one renders into `finalTarget`. `step`: advance
    /// persistent passes (false reuses their last state; the first frame always steps).
    func encode(into cmd: MTLCommandBuffer, finalTarget: MTLTexture, delta: Double, step: Bool = true) {
        let size = CGSize(width: finalTarget.width, height: finalTarget.height)
        ensureTargets(for: size)
        frame += 1

        // Persistent state starts as transparent black.
        for texture in needsClear {
            let rpd = MTLRenderPassDescriptor()
            rpd.colorAttachments[0].texture = texture
            rpd.colorAttachments[0].loadAction = .clear
            rpd.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 0)
            rpd.colorAttachments[0].storeAction = .store
            cmd.makeRenderCommandEncoder(descriptor: rpd)?.endEncoding()
        }
        needsClear = []
        let reset = steps == 0
        let step = step || reset
        // Time since this screen's last step; the clock may wrap (spec), and a long gap is capped.
        var stepDelta = reset ? delta : time - lastStepTime
        if stepDelta < 0 { stepDelta = delta }
        stepDelta = min(stepDelta, 0.1)
        let state = SIMD4<Float>(reset ? 1 : 0, Float(steps), Float(stepDelta), 0)

        var uniforms = [SIMD4<Float>](repeating: .zero, count: 3)
        uniforms[0] = SIMD4(Float(time), Float(frame), Float(delta), Float(wallpaper.mipmapLevelCount))
        uniforms[2] = SIMD4(Float(size.width), Float(size.height),
                            Float(wallpaper.width), Float(wallpaper.height))
        uniforms.append(contentsOf: params)
        uniforms.append(.zero)   // uState; 3 + 16 + 1 vec4 = 320 bytes, as in the spec
        let stateIndex = uniforms.count - 1

        var previous: MTLTexture = wallpaper
        let last = compiled.pipelines.count - 1
        let write = 1 - cur
        for (i, pipeline) in compiled.pipelines.enumerated() {
            let persistent = compiled.effect.isPersistent(i)
            if persistent && !step {   // still frame: reuse the latest state
                previous = targets[i][cur]
                continue
            }
            let target: MTLTexture = (i == last) ? finalTarget
                : (targets[i].isEmpty ? finalTarget : targets[i][persistent ? write : 0])
            uniforms[1] = SIMD4(Float(target.width), Float(target.height),
                                1 / Float(target.width), 1 / Float(target.height))
            uniforms[stateIndex] = persistent ? state : .zero

            let rpd = MTLRenderPassDescriptor()
            rpd.colorAttachments[0].texture = target
            rpd.colorAttachments[0].loadAction = .dontCare
            rpd.colorAttachments[0].storeAction = .store
            guard let enc = cmd.makeRenderCommandEncoder(descriptor: rpd) else { return }
            enc.setRenderPipelineState(pipeline)
            uniforms.withUnsafeBytes { enc.setFragmentBytes($0.baseAddress!, length: $0.count, index: 0) }
            enc.setFragmentTexture(wallpaper, index: 1)
            enc.setFragmentSamplerState(gpu.sampler, index: 1)
            enc.setFragmentTexture(previous, index: 2)
            enc.setFragmentSamplerState(gpu.sampler, index: 2)
            enc.setFragmentTexture(persistent ? targets[i][cur] : gpu.blackTexture, index: 3)
            enc.setFragmentSamplerState(gpu.sampler, index: 3)
            enc.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)
            enc.endEncoding()
            previous = target
        }
        if step {
            cur = write
            steps += 1
            lastStepTime = time
        }
    }
}
