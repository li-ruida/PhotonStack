import Foundation
import Testing
@testable import PhotonStackUI

@Test func backgroundProtectionRecipePreservesRotationAndRejectsPartialInput() throws {
    let recipe = "1100,1950,1350,520,60;1560,1460,210,140,105"
    let ellipses = try #require(BackgroundProtectionEllipse.parse(recipe))
    #expect(ellipses.count == 2)
    #expect(BackgroundProtectionEllipse.parse(BackgroundProtectionEllipse.recipe(ellipses)) == ellipses)
    #expect(BackgroundProtectionEllipse.parse("") == [])
    for invalid in ["1,2,3,4", "1,2,3,4,0;bad", "1,2,0,4,0", "1,2,-3,4,0", "1,2,3,4,nan", "inf,2,3,4,0"] {
        #expect(BackgroundProtectionEllipse.parse(invalid) == nil)
    }
    #expect(ellipses[0].contains(CGPoint(x: 1100, y: 1950)))
    let rotated = BackgroundProtectionEllipse(x: 100, y: 200, a: 50, b: 10, angle: 90)
    #expect(rotated.contains(CGPoint(x: 100, y: 240)))
    #expect(!rotated.contains(CGPoint(x: 140, y: 200)))
}

@Test func backgroundProtectionCroppedPortraitMappingAndDrawing() throws {
    // Fits into the middle of a wide editor, not the complete editor rectangle.
    let crop = CGRect(x: 127, y: 108, width: 1956, height: 3618)
    for size in [CGSize(width: 600, height: 460), CGSize(width: 1200, height: 920)] {
        let geometry = BackgroundProtectionGeometry(crop: crop, size: size)
        let center = CGPoint(x: 1080, y: 1880)
        let displayed = geometry.canvasPoint(center)
        let restored = geometry.masterPoint(displayed)
        #expect(abs(restored.x - center.x) < 1e-9)
        #expect(abs(restored.y - center.y) < 1e-9)
        let start = geometry.canvasPoint(CGPoint(x: 840, y: 1640))
        let end = geometry.canvasPoint(CGPoint(x: 1320, y: 2120))
        for (a, b) in [(start, end), (end, start)] {
            let ellipse = try #require(geometry.drawn(from: a, to: b))
            #expect(abs(ellipse.x - 1080) < 1e-9)
            #expect(abs(ellipse.y - 1880) < 1e-9)
            #expect(abs(ellipse.a - 240) < 1e-9)
            #expect(abs(ellipse.b - 240) < 1e-9)
        }
        #expect(geometry.drawn(from: .zero, to: displayed) == nil) // Letterbox is not image data.
        #expect(geometry.drawn(from: displayed, to: displayed) == nil)
        let clipped = try #require(geometry.drawn(from: displayed, to: CGPoint(x: size.width * 2, y: size.height * 2)))
        #expect(abs(clipped.x + clipped.a - (crop.maxX - 0.5)) < 1e-9)
        #expect(abs(clipped.y + clipped.b - (crop.maxY - 0.5)) < 1e-9)
        let firstPixel = geometry.canvasPoint(CGPoint(x: crop.minX, y: crop.minY))
        #expect(abs(firstPixel.x - geometry.imageRect.minX - geometry.scale / 2) < 1e-9)
    }
}
