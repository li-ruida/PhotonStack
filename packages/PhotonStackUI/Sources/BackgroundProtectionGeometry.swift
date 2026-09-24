import Foundation

/// Recipe coordinates use pixel centers in the uncropped reference canvas.
struct BackgroundProtectionEllipse: Equatable {
    var x, y, a, b, angle: Double
    var valid: Bool { [x, y, a, b, angle].allSatisfy { $0.isFinite && abs($0) < Double(Float.greatestFiniteMagnitude) } && a > 0 && b > 0 }

    func contains(_ point: CGPoint) -> Bool {
        guard valid else { return false }
        let dx = point.x - x, dy = point.y - y, radians = angle * .pi / 180
        let u = (dx * cos(radians) + dy * sin(radians)) / a
        let v = (-dx * sin(radians) + dy * cos(radians)) / b
        return u * u + v * v <= 1
    }

    static func parse(_ text: String) -> [Self]? {
        if text.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty { return [] }
        var result: [Self] = []
        for entry in text.split(separator: ";", omittingEmptySubsequences: false) {
            let fields = entry.split(separator: ",", omittingEmptySubsequences: false)
            let values = fields.compactMap { Double($0.trimmingCharacters(in: .whitespacesAndNewlines)) }
            guard fields.count == 5, values.count == 5 else { return nil }
            let ellipse = Self(x: values[0], y: values[1], a: values[2], b: values[3], angle: values[4])
            guard ellipse.valid else { return nil }
            result.append(ellipse)
        }
        return result
    }

    static func recipe(_ ellipses: [Self]) -> String {
        ellipses.map { [$0.x, $0.y, $0.a, $0.b, $0.angle].map { String($0) }.joined(separator: ",") }.joined(separator: ";")
    }
}

struct BackgroundProtectionGeometry {
    let crop: CGRect
    let size: CGSize
    var scale: Double { min(size.width / crop.width, size.height / crop.height) }
    var imageRect: CGRect {
        let w = crop.width * scale, h = crop.height * scale
        return CGRect(x: (size.width - w) / 2, y: (size.height - h) / 2, width: w, height: h)
    }
    func canvasPoint(_ point: CGPoint) -> CGPoint {
        CGPoint(x: imageRect.minX + (point.x - crop.minX + 0.5) * scale,
                y: imageRect.minY + (point.y - crop.minY + 0.5) * scale)
    }
    func masterPoint(_ point: CGPoint) -> CGPoint {
        CGPoint(x: (point.x - imageRect.minX) / scale + crop.minX - 0.5,
                y: (point.y - imageRect.minY) / scale + crop.minY - 0.5)
    }
    func drawn(from start: CGPoint, to end: CGPoint) -> BackgroundProtectionEllipse? {
        guard imageRect.contains(start) else { return nil }
        let bounded = CGPoint(x: min(max(end.x, imageRect.minX), imageRect.maxX),
                              y: min(max(end.y, imageRect.minY), imageRect.maxY))
        guard abs(start.x - bounded.x) >= 4, abs(start.y - bounded.y) >= 4 else { return nil }
        let p = masterPoint(start), q = masterPoint(bounded)
        return BackgroundProtectionEllipse(x: (p.x + q.x) / 2, y: (p.y + q.y) / 2,
                                           a: abs(p.x - q.x) / 2, b: abs(p.y - q.y) / 2, angle: 0)
    }
}
