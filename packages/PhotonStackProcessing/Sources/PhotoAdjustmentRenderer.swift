import CoreImage
import Foundation
import ImageIO
import PhotonStackAppCore

/// Display diagnostics are deliberately separate from the saved photo recipe.
public struct PhotoClippingWarnings: OptionSet, Sendable {
    public let rawValue: Int
    public init(rawValue: Int) { self.rawValue = rawValue }
    public static let shadows = Self(rawValue: 1)
    public static let highlights = Self(rawValue: 2)
}

/// An editor retains one downsampled, oriented source. Export uses the same
/// filter chain on the full-resolution image in Core Image's linear working space.
public actor PhotoAdjustmentRenderer {
    private static let context = CIContext(options: [.cacheIntermediates: false])
    private let colorSpace = CGColorSpace(name: CGColorSpace.sRGB)!
    private var thumbnail: (url: URL, image: CIImage, sourceSize: CGSize)?

    private var fullSource: (url: URL, image: CIImage)?

    public init() {}

    private func fullImage(at url: URL) throws -> CIImage {
        if let fullSource, fullSource.url == url { return fullSource.image }
        guard let image = CIImage(contentsOf: url, options: [.applyOrientationProperty: true]) else {
            throw CocoaError(.fileReadCorruptFile)
        }
        fullSource = (url, image)
        return image
    }

    /// The point is normalized to the displayed crop, measured from its top left.
    /// Sample a five-by-five footprint from the original, in linear sRGB.
    public func neutralSample(input: URL, adjustments: PhotoAdjustments, at point: CGPoint) throws -> PhotoAdjustments.NeutralSample? {
        guard (0...1).contains(point.x), (0...1).contains(point.y) else { return nil }
        try Task.checkCancellation()
        let source = geometry(try fullImage(at: input), adjustments)
        let bounds = source.extent
        let x = min(floor(point.x * bounds.width), bounds.width - 1)
        let y = bounds.height - 1 - min(floor(point.y * bounds.height), bounds.height - 1)
        let area = CGRect(x: bounds.minX + x - 2, y: bounds.minY + y - 2, width: 5, height: 5).intersection(bounds)
        let count = Int(area.width * area.height)
        guard count > 0 else { return nil }
        var pixels = [Float](repeating: 0, count: count * 4)
        Self.context.render(source, toBitmap: &pixels, rowBytes: Int(area.width) * 16,
                            bounds: area, format: .RGBAf, colorSpace: CGColorSpace(name: CGColorSpace.extendedLinearSRGB)!)
        var sum = SIMD3<Double>.zero
        var accepted = 0
        for offset in stride(from: 0, to: pixels.count, by: 4) {
            guard pixels[offset + 3] >= 0.999 else { continue }
            let rgb = SIMD3(Double(pixels[offset]), Double(pixels[offset + 1]), Double(pixels[offset + 2]))
            guard rgb.min() >= 0.0003, rgb.max() < 0.99 else { continue }
            sum += rgb
            accepted += 1
        }
        guard accepted * 2 > count else { return nil }
        let mean = sum / Double(accepted)
        return PhotoAdjustments.NeutralSample(red: mean.x, green: mean.y, blue: mean.z)
    }

    public func preview(input: URL, adjustments: PhotoAdjustments, warnings: PhotoClippingWarnings = []) throws -> (image: CGImage, sourceSize: CGSize) {
        try Task.checkCancellation()
        if !warnings.isEmpty {
            let full = filtered(try fullImage(at: input), adjustments)
            let image = clippingPreview(full, warnings: warnings)
            guard let result = Self.context.createCGImage(image, from: image.extent, format: .RGBA8, colorSpace: colorSpace)
            else { throw CocoaError(.fileReadCorruptFile) }
            return (result, full.extent.size)
        }
        if thumbnail?.url != input {
            let options: [CFString: Any] = [
                kCGImageSourceCreateThumbnailFromImageAlways: true,
                kCGImageSourceCreateThumbnailWithTransform: true,
                kCGImageSourceThumbnailMaxPixelSize: 1400,
                kCGImageSourceShouldCacheImmediately: true,
            ]
            guard let source = CGImageSourceCreateWithURL(input as CFURL, nil),
                  let bitmap = CGImageSourceCreateThumbnailAtIndex(source, 0, options as CFDictionary) else {
                throw CocoaError(.fileReadCorruptFile)
            }
            let sourceSize = Self.orientedSize(of: source) ?? CGSize(width: bitmap.width, height: bitmap.height)
            thumbnail = (input, CIImage(cgImage: bitmap), sourceSize)
        }
        try Task.checkCancellation()
        guard let thumbnail else { throw CocoaError(.fileReadCorruptFile) }
        let image = filtered(thumbnail.image, adjustments)
        guard let result = Self.context.createCGImage(image, from: image.extent, format: .RGBA8, colorSpace: colorSpace)
        else { throw CocoaError(.fileReadCorruptFile) }
        let size = thumbnail.sourceSize
        let orientedSize = adjustments.quarterTurns.isMultiple(of: 2) ? size : CGSize(width: size.height, height: size.width)
        return (result, adjustments.cropRect(in: orientedSize).size)
    }

    private func clippingPreview(_ image: CIImage, warnings: PhotoClippingWarnings) -> CIImage {
        let size = image.extent.size
        let scale = min(1, 1400 / max(size.width, size.height))
        let transform = CGAffineTransform(scaleX: max(1, (size.width * scale).rounded(.down)) / size.width,
                                          y: max(1, (size.height * scale).rounded(.down)) / size.height)
        let base = image.transformed(by: transform, highQualityDownsample: true)
        var display = base
        let maximum = image.applyingFilter("CIMaximumComponent")
        // Classify before downsampling so a clipped star core can survive a fit-to-window preview.
        func overlay(_ mask: CIImage, color: CIColor, preserveSmallCores: Bool = false, on background: CIImage) -> CIImage {
            let visible = preserveSmallCores && scale < 1 ? mask.applyingFilter("CIMorphologyMaximum",
                parameters: [kCIInputRadiusKey: ceil(0.5 / scale)]) : mask
            let scaled = visible.transformed(by: transform, highQualityDownsample: !preserveSmallCores)
            let marker = CIImage(color: color).applyingFilter("CISourceInCompositing", parameters: [kCIInputBackgroundImageKey: base])
            return marker.applyingFilter("CIBlendWithMask", parameters: [
                kCIInputBackgroundImageKey: background, kCIInputMaskImageKey: scaled
            ]).cropped(to: background.extent)
        }
        if warnings.contains(.shadows) {
            // In linear sRGB, values below this threshold round to black in a 16-bit sRGB TIFF.
            let nonBlack = maximum.applyingFilter("CIColorThreshold", parameters: ["inputThreshold": 0.5 / (65535 * 12.92)])
            display = overlay(nonBlack.applyingFilter("CIColorInvert"), color: .blue, on: display)
        }
        if warnings.contains(.highlights) {
            let white = maximum.applyingFilter("CIColorThreshold", parameters: ["inputThreshold": 1.0])
            display = overlay(white, color: .red, preserveSmallCores: true, on: display)
        }
        return display
    }

    public static func sourceSize(at input: URL) async -> CGSize? {
        guard let source = CGImageSourceCreateWithURL(input as CFURL, nil) else { return nil }
        return orientedSize(of: source)
    }

    private static func orientedSize(of source: CGImageSource) -> CGSize? {
        guard let properties = CGImageSourceCopyPropertiesAtIndex(source, 0, nil) as? [CFString: Any],
              let width = properties[kCGImagePropertyPixelWidth] as? Int,
              let height = properties[kCGImagePropertyPixelHeight] as? Int else { return nil }
        let orientation = properties[kCGImagePropertyOrientation] as? Int ?? 1
        return (5...8).contains(orientation) ? CGSize(width: height, height: width) : CGSize(width: width, height: height)
    }

    public func writeTIFF(input: URL, output: URL, adjustments: PhotoAdjustments) throws {
        try Task.checkCancellation()
        guard let source = CIImage(contentsOf: input, options: [.applyOrientationProperty: true]) else {
            throw CocoaError(.fileReadCorruptFile)
        }
        let staged = output.deletingLastPathComponent().appendingPathComponent(UUID().uuidString + ".tiff")
        defer { try? FileManager.default.removeItem(at: staged) }
        try Self.context.writeTIFFRepresentation(of: filtered(source, adjustments), to: staged,
                                            format: .RGBA16, colorSpace: colorSpace)
        try Task.checkCancellation()
        if FileManager.default.fileExists(atPath: output.path) {
            _ = try FileManager.default.replaceItemAt(output, withItemAt: staged)
        } else {
            try FileManager.default.moveItem(at: staged, to: output)
        }
    }

    private func geometry(_ source: CIImage, _ settings: PhotoAdjustments) -> CIImage {
        var image = source
        let orientations: [CGImagePropertyOrientation] = [.up, .right, .down, .left]
        if settings.quarterTurns != 0 { image = image.oriented(orientations[settings.quarterTurns]) }
        if settings.mirror { image = image.oriented(.upMirrored) }
        if settings.flipVertical { image = image.oriented(.downMirrored) }
        if settings.aspect != .original || settings.cropScale != 1 {
            let extent = image.extent
            let rect = settings.cropRect(in: extent.size).offsetBy(dx: extent.minX, dy: extent.minY)
            image = image.cropped(to: rect).transformed(by: .init(translationX: -rect.minX, y: -rect.minY))
        }
        return image
    }

    private func filtered(_ source: CIImage, _ settings: PhotoAdjustments) -> CIImage {
        var image = geometry(source, settings)
        if let sample = settings.neutralSample {
            let target = sample.luminance
            image = image.applyingFilter("CIColorMatrix", parameters: [
                "inputRVector": CIVector(x: target / sample.red, y: 0, z: 0, w: 0),
                "inputGVector": CIVector(x: 0, y: target / sample.green, z: 0, w: 0),
                "inputBVector": CIVector(x: 0, y: 0, z: target / sample.blue, w: 0)
            ])
        }
        if settings[.exposure] != 0 {
            image = image.applyingFilter("CIExposureAdjust", parameters: [kCIInputEVKey: settings[.exposure]])
        }
        if settings[.warmth] != 0 || settings[.tint] != 0 {
            let warm = settings[.warmth] * 0.6
            let tint = settings[.tint] * 0.4
            let red = exp2(warm + tint / 2)
            let green = exp2(-tint)
            let blue = exp2(-warm + tint / 2)
            let luminance = 0.2126 * red + 0.7152 * green + 0.0722 * blue
            image = image.applyingFilter("CIColorMatrix", parameters: [
                "inputRVector": CIVector(x: red / luminance, y: 0, z: 0, w: 0),
                "inputGVector": CIVector(x: 0, y: green / luminance, z: 0, w: 0),
                "inputBVector": CIVector(x: 0, y: 0, z: blue / luminance, w: 0),
            ])
        }
        if settings[.shadows] != 0 || settings[.highlights] != 0 || settings[.contrast] != 0 {
            // Ordered control points keep the tone transfer monotonic even at the limits.
            let contrast = 0.09 * settings[.contrast]
            image = image.applyingFilter("CIToneCurve", parameters: [
                "inputPoint1": CIVector(x: 0.25, y: 0.25 + 0.14 * settings[.shadows] - contrast),
                "inputPoint3": CIVector(x: 0.75, y: 0.75 + 0.14 * settings[.highlights] + contrast),
            ])
        }
        if settings[.vibrance] != 0 {
            image = image.applyingFilter("CIVibrance", parameters: ["inputAmount": settings[.vibrance]])
        }
        if settings[.saturation] != 0 {
            image = image.applyingFilter("CIColorControls", parameters: [kCIInputSaturationKey: 1 + settings[.saturation]])
        }
        if settings[.blackPoint] != 0 || settings[.whitePoint] != 255 {
            // Levels are displayed in sRGB code values even for 16-bit inputs.
            let gain = 255 / (settings[.whitePoint] - settings[.blackPoint])
            let bias = -settings[.blackPoint] / 255 * gain
            image = image.applyingFilter("CILinearToSRGBToneCurve")
                .applyingFilter("CIColorMatrix", parameters: [
                    "inputRVector": CIVector(x: gain, y: 0, z: 0, w: 0),
                    "inputGVector": CIVector(x: 0, y: gain, z: 0, w: 0),
                    "inputBVector": CIVector(x: 0, y: 0, z: gain, w: 0),
                    "inputBiasVector": CIVector(x: bias, y: bias, z: bias, w: 0)
                ]).applyingFilter("CIColorClamp")
                .applyingFilter("CISRGBToneCurveToLinear")
        }
        return image
    }
}
