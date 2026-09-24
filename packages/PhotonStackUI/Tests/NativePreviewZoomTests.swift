#if os(macOS)
import AppKit
import Testing
import ImageIO
@testable import PhotonStackUI

@Test func nativePreviewActualSizeMapsSourcePixelsToScreenPixels() {
    // A 320px-wide crop must occupy 320 physical pixels at 1:1 on either display.
    for scale in [1.0, 2.0] {
        let scroll = NativePreviewZoom.scrollMagnification(pixelMagnification: 1, backingScale: scale)
        #expect(abs(320 * scroll * scale - 320) < 1e-10)
        let doubleSize = NativePreviewZoom.scrollMagnification(pixelMagnification: 2, backingScale: scale)
        #expect(abs(320 * doubleSize * scale - 640) < 1e-10)
    }
}

@Test func nativePreviewFitAndGestureZoomUsePhysicalPixels() {
    // The same 400pt-wide viewport holds a 1600px image at 25% on a 1x
    // monitor or 50% on a 2x monitor, without changing its fitted layout.
    for (scale, expectedLabel) in [(1.0, 0.25), (2.0, 0.5)] {
        let fitted = NativePreviewZoom.pixelMagnification(scrollMagnification: 400.0 / 1600, backingScale: scale)
        #expect(fitted == expectedLabel)
        #expect(NativePreviewZoom.scrollMagnification(pixelMagnification: fitted, backingScale: scale) * 1600 == 400)
        let pinched = NativePreviewZoom.pixelMagnification(scrollMagnification: 0.75, backingScale: scale)
        #expect(pinched == 0.75 * scale)
    }
}

@Test @MainActor func nativePreviewCoordinatorUsesBackingScaleForActualSize() async throws {
    let imageView = NSImageView(frame: CGRect(x: 0, y: 0, width: 1600, height: 1200))
    let scrollView = NativePreviewScrollView(frame: CGRect(x: 0, y: 0, width: 400, height: 300))
    scrollView.allowsMagnification = true
    scrollView.documentView = imageView
    let coordinator = NativeImagePreview.Coordinator()
    coordinator.scrollView = scrollView
    coordinator.imageView = imageView
    coordinator.naturalImageSize = CGSize(width: 1600, height: 1200)
    let scale = NativeImagePreview.Coordinator.backingScale(for: scrollView)
    coordinator.applyZoom(magnification: 1, fitToWindow: false, updateMagnification: { _ in })
    #expect(abs(Double(scrollView.magnification) * scale - 1) < 1e-6)
    coordinator.applyZoom(magnification: 1, fitToWindow: true, updateMagnification: { _ in })
    #expect(abs(Double(scrollView.magnification) - 0.25) < 1e-6)
    // A pending fit after import must not undo a subsequent 1:1 click.
    coordinator.scheduleDeferredFitZoom(magnification: 1, updateMagnification: { _ in })
    coordinator.applyZoom(magnification: 1, fitToWindow: false, updateMagnification: { _ in })
    try await Task.sleep(nanoseconds: 100_000_000)
    #expect(abs(Double(scrollView.magnification) * scale - 1) < 1e-6)
}
private func writePreviewFixture(_ url: URL, width: Int) throws {
    let context = try #require(CGContext(data: nil, width: width, height: 16, bitsPerComponent: 8,
        bytesPerRow: width * 4, space: CGColorSpaceCreateDeviceRGB(),
        bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue))
    context.setFillColor(CGColor(red: 0.3, green: 0.5, blue: 0.7, alpha: 1))
    context.fill(CGRect(x: 0, y: 0, width: width, height: 16))
    let image = try #require(context.makeImage())
    let destination = try #require(CGImageDestinationCreateWithURL(url as CFURL, "public.png" as CFString, 1, nil))
    CGImageDestinationAddImage(destination, image, nil)
    #expect(CGImageDestinationFinalize(destination))
}

@Test func nativePreviewLoaderReusesPixelsAndInvalidatesChangedFiles() async throws {
    let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
    try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
    defer { try? FileManager.default.removeItem(at: root) }
    let urls = (0..<3).map { root.appendingPathComponent("\($0).png") }
    for (i, url) in urls.enumerated() { try writePreviewFixture(url, width: 32 + i) }
    let loader = NativePreviewImageLoader()
    let first = try #require(try await loader.load(urls[0]))
    _ = try await loader.load(urls[1])
    let reused = try #require(try await loader.load(urls[0]))
    #expect(first === reused)
    _ = try await loader.load(urls[2])
    #expect(try await loader.load(urls[0]) === first) // true LRU, not insertion order
    try writePreviewFixture(urls[0], width: 64)
    let changed = try #require(try await loader.load(urls[0]))
    #expect(changed.width == 64 && changed !== first)
    try FileManager.default.removeItem(at: urls[0])
    await #expect(throws: (any Error).self) { try await loader.load(urls[0]) }
}

@Test @MainActor func nativePreviewRapidSwitchOnlyPublishesLatestImage() async throws {
    let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
    try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
    defer { try? FileManager.default.removeItem(at: root) }
    let first = root.appendingPathComponent("first.png"), last = root.appendingPathComponent("last.png")
    try writePreviewFixture(first, width: 32); try writePreviewFixture(last, width: 64)
    let coordinator = NativeImagePreview.Coordinator()
    let imageView = NSImageView()
    coordinator.imageView = imageView
    var loadedSizes: [CGFloat] = []
    coordinator.imageLoaded = { loadedSizes.append(imageView.image?.size.width ?? 0) }
    coordinator.updateImage(first)
    coordinator.updateImage(last)
    for _ in 0..<100 where loadedSizes.isEmpty { try await Task.sleep(for: .milliseconds(10)) }
    #expect(loadedSizes == [64])
    #expect(coordinator.url == last)
    coordinator.updateImage(first)
    coordinator.cancelImageLoad()
    try await Task.sleep(for: .milliseconds(50))
    #expect(loadedSizes == [64]) // closing/switching cannot publish an abandoned decode
}
@Test @MainActor func nativePreviewComparisonSwitchPreservesPanAndZoom() throws {
    let firstContext = try #require(CGContext(data: nil, width: 800, height: 600, bitsPerComponent: 8,
        bytesPerRow: 800 * 4, space: CGColorSpaceCreateDeviceRGB(), bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue))
    firstContext.setFillColor(CGColor(red: 1, green: 0, blue: 0, alpha: 1))
    firstContext.fill(CGRect(x: 0, y: 0, width: 800, height: 600))
    let first = try #require(firstContext.makeImage())
    firstContext.setFillColor(CGColor(red: 0, green: 1, blue: 0, alpha: 1))
    firstContext.fill(CGRect(x: 0, y: 0, width: 800, height: 600))
    let second = try #require(firstContext.makeImage())
    #expect(ResultComparisonPair.validate(current: first, reference: second))
    let smallContext = try #require(CGContext(data: nil, width: 80, height: 60, bitsPerComponent: 8,
        bytesPerRow: 80 * 4, space: CGColorSpaceCreateDeviceRGB(), bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue))
    #expect(!ResultComparisonPair.validate(current: first, reference: try #require(smallContext.makeImage())))
    let view = NSImageView(frame: CGRect(x: 0, y: 0, width: 800, height: 600))
    let scroll = NativePreviewScrollView(frame: CGRect(x: 0, y: 0, width: 300, height: 200))
    scroll.allowsMagnification = true; scroll.documentView = view
    let coordinator = NativeImagePreview.Coordinator(); coordinator.scrollView = scroll; coordinator.imageView = view
    let a = URL(fileURLWithPath: "/comparison/a.png"), b = URL(fileURLWithPath: "/comparison/b.png")
    coordinator.updateImage(a, decodedImage: first)
    coordinator.applyZoom(magnification: 2, fitToWindow: false, updateMagnification: { _ in })
    scroll.contentView.scroll(to: NSPoint(x: 170, y: 120))
    let bounds = scroll.contentView.bounds, zoom = scroll.magnification
    for index in 0..<10 {
        coordinator.updateImage(index.isMultiple(of: 2) ? b : a, decodedImage: index.isMultiple(of: 2) ? second : first)
        #expect(scroll.contentView.bounds == bounds)
        #expect(scroll.magnification == zoom)
        #expect(view.image != nil) // decoded switches publish synchronously, without a blank frame
        let pixels = try #require(view.image?.cgImage(forProposedRect: nil, context: nil, hints: nil))
        let color = try #require(NSBitmapImageRep(cgImage: pixels).colorAt(x: 0, y: 0)?.usingColorSpace(.deviceRGB))
        // Device RGB can color-manage pure sRGB primaries below 1.0. Check
        // which image was published without assuming a monitor profile.
        #expect(index.isMultiple(of: 2)
            ? color.greenComponent - color.redComponent > 0.8
            : color.redComponent - color.greenComponent > 0.8)
        #expect(coordinator.naturalImageSize == CGSize(width: 800, height: 600))
    }
}

@Test @MainActor func nativePreviewComparisonCentersOnlyTheFirstImage() throws {
    let context = try #require(CGContext(data: nil, width: 800, height: 600, bitsPerComponent: 8,
        bytesPerRow: 3200, space: CGColorSpaceCreateDeviceRGB(), bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue))
    let image = try #require(context.makeImage())
    let view = NSImageView(), scroll = NativePreviewScrollView(frame: CGRect(x: 0, y: 0, width: 300, height: 200))
    scroll.allowsMagnification = true; scroll.documentView = view
    let coordinator = NativeImagePreview.Coordinator()
    coordinator.scrollView = scroll; coordinator.imageView = view; coordinator.centerFirstImage = true
    coordinator.updateImage(URL(fileURLWithPath: "/comparison/a.tiff"), decodedImage: image)
    coordinator.applyZoom(magnification: 1, fitToWindow: false, updateMagnification: { _ in })
    #expect(abs(scroll.contentView.bounds.midX - 400) < 1)
    #expect(abs(scroll.contentView.bounds.midY - 300) < 1)
    scroll.contentView.scroll(to: NSPoint(x: 17, y: 29))
    let panned = scroll.contentView.bounds
    coordinator.updateImage(URL(fileURLWithPath: "/comparison/b.tiff"), decodedImage: image)
    coordinator.centerInitialImageIfNeeded()
    coordinator.applyZoom(magnification: 1, fitToWindow: false, updateMagnification: { _ in })
    #expect(scroll.contentView.bounds == panned)
}

@Test @MainActor func nativePreviewMagnificationPreservesIndividualPixelValues() throws {
    let context = try #require(CGContext(data: nil, width: 32, height: 32, bitsPerComponent: 8,
        bytesPerRow: 128, space: CGColorSpaceCreateDeviceRGB(),
        bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue))
    for y in 0..<32 { for x in 0..<32 {
        context.setFillColor(CGColor(gray: (x + y).isMultiple(of: 2) ? 0 : 1, alpha: 1))
        context.fill(CGRect(x: x, y: y, width: 1, height: 1))
    } }
    let image = NSImage(cgImage: try #require(context.makeImage()), size: CGSize(width: 32, height: 32))
    for scale in [1, 2, 3] {
        let view = NativePreviewImageView(frame: CGRect(x: 0, y: 0, width: 32 * scale, height: 32 * scale))
        view.imageScaling = .scaleProportionallyUpOrDown
        view.image = image
        view.usesNearestNeighbor = true
        let bitmap = try #require(NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: 32 * scale,
            pixelsHigh: 32 * scale, bitsPerSample: 8, samplesPerPixel: 4, hasAlpha: true, isPlanar: false,
            colorSpaceName: .deviceRGB, bytesPerRow: 128 * scale, bitsPerPixel: 32))
        view.cacheDisplay(in: view.bounds, to: bitmap)
        // Inspect every sample inside the source, not just dimensions or zoom labels.
        for y in 8 * scale..<24 * scale { for x in 8 * scale..<24 * scale {
            let value = try #require(bitmap.colorAt(x: x, y: y)).redComponent
            // CGContext fixture coordinates are bottom-up; bitmap rows are top-down.
            let expected: CGFloat = ((x / scale) + (31 - y / scale)).isMultiple(of: 2) ? 0 : 1
            #expect(abs(value - expected) < 0.02)
        } }
    }
}

@Test @MainActor func nativePreviewOnlyDisablesInterpolationAtPhysicalNativeScale() {
    let view = NativePreviewImageView(frame: CGRect(x: 0, y: 0, width: 1600, height: 1200))
    view.wantsLayer = true
    let scroll = NativePreviewScrollView(frame: CGRect(x: 0, y: 0, width: 400, height: 300))
    scroll.allowsMagnification = true
    scroll.documentView = view
    let coordinator = NativeImagePreview.Coordinator()
    coordinator.scrollView = scroll
    coordinator.imageView = view
    coordinator.naturalImageSize = view.frame.size
    for scale in [0.5, 1.0, 2.0, 0.8] {
        coordinator.applyZoom(magnification: scale, fitToWindow: false, updateMagnification: { _ in })
        #expect(view.usesNearestNeighbor == (scale >= 1))
        #expect(view.layer?.magnificationFilter == (scale >= 1 ? .nearest : .linear))
        #expect(view.layer?.minificationFilter == (scale >= 1 ? .nearest : .linear))
    }
}
#endif
