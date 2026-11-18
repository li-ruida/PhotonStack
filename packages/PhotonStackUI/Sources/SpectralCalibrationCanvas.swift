import AppKit
import SwiftUI

func spectralStarStatus(_ status: String, chinese: Bool) -> String {
    guard chinese else { return status.replacingOccurrences(of: "-", with: " ") }
    if status.hasPrefix("expanded-") { return "扩大孔径：" + spectralStarStatus(String(status.dropFirst(9)), chinese: true) }
    return ["fit": "拟合星", "validation": "独立留出星", "fit-outlier": "拟合离群", "excluded": "手动排除",
            "outside": "画幅外", "catalog-neighbor": "星表近邻", "detected-neighbor": "检测到近邻",
            "spectral-error-missing": "缺少光谱误差", "spectral-error": "光谱误差过大", "edge": "靠近边缘",
            "centroid-fit": "质心拟合失败", "position-mismatch": "位置不匹配", "coverage": "覆盖不足",
            "saturation": "达到饱和值", "clipped-channel": "单通道截断", "sky-fit": "背景拟合失败",
            "sky-samples": "背景样本不足", "signal-to-noise": "信噪比不足", "detection-mismatch": "未匹配检测星",
            "aperture-color-unstable": "孔径颜色不稳定", "eligible": "待拟合"][status] ?? status
}
func spectralStarColor(_ star: SpectralCalibrationReport.Star) -> Color {
    star.status == "validation" ? .cyan : star.status == "fit" ? .yellow : .orange
}
struct SpectralCalibrationCanvas: View {
    let report: SpectralCalibrationReport
    let image: NSImage
    let showStars: Bool
    @Binding var selected: String?
    var body: some View {
        GeometryReader { geometry in
            let scale = min(geometry.size.width/Double(report.width), geometry.size.height/Double(report.height))
            let size = CGSize(width: Double(report.width)*scale, height: Double(report.height)*scale)
            let origin = CGPoint(x: (geometry.size.width-size.width)/2, y: (geometry.size.height-size.height)/2)
            Canvas { context, _ in
                context.draw(Image(nsImage: image), in: CGRect(origin: origin, size: size))
                for star in report.stars where showStars || star.id == selected {
                    guard let (x,y) = star.position, x >= 0, y >= 0, x < Double(report.width), y < Double(report.height) else { continue }
                    let point = CGPoint(x: origin.x+(x+0.5)*scale, y: origin.y+(y+0.5)*scale)
                    let radius = star.id == selected ? 8.0 : 3.5
                    context.stroke(Path(ellipseIn: CGRect(x: point.x-radius, y: point.y-radius, width: radius*2, height: radius*2)),
                                   with: .color(star.id == selected ? .white : spectralStarColor(star)), lineWidth: star.id == selected ? 2 : 1)
                }
            }.background(.black)
                .gesture(SpatialTapGesture().onEnded { value in
                    let candidates = report.stars.compactMap { star -> (String, Double)? in
                        guard let (x,y) = star.position, x >= 0, y >= 0, x < Double(report.width), y < Double(report.height) else { return nil }
                        return (star.id, hypot(origin.x+(x+0.5)*scale-value.location.x, origin.y+(y+0.5)*scale-value.location.y))
                    }
                    selected = candidates.min { $0.1 < $1.1 }.flatMap { $0.1 <= 12 ? $0.0 : nil }
                })
                .accessibilityLabel("Spectral calibration star review preview")
        }
    }
}
struct SpectralResidualPlot: View {
    let report: SpectralCalibrationReport
    let before: Bool
    let selected: String?
    var body: some View {
        Canvas { context, size in
            let entries: [(SpectralCalibrationReport.Star, Double, Double)] = report.stars.compactMap { s in
                guard let r = s.residual, let x = r[0], let y = r[2], let gr = report.fit.gains[0], let gb = report.fit.gains[2], gr > 0, gb > 0 else { return nil }
                return (s, x-(before ? log(gr) : 0), y-(before ? log(gb) : 0))
            }
            // Use one scale for both comparison states so apparent scatter
            // cannot improve simply because the plot axes changed.
            let limit = max(0.2, report.stars.compactMap { s -> Double? in
                guard let r = s.residual, let x = r[0], let y = r[2], let gr = report.fit.gains[0], let gb = report.fit.gains[2], gr > 0, gb > 0 else { return nil }
                return max(abs(x), abs(y), abs(x-log(gr)), abs(y-log(gb)))
            }.max() ?? 0.2)
            let rect = CGRect(x: 40, y: 8, width: max(1,size.width-80), height: max(1,size.height-30))
            func point(_ x: Double, _ y: Double) -> CGPoint { CGPoint(x: rect.midX+x/limit*rect.width/2, y: rect.midY-y/limit*rect.height/2) }
            var axes = Path(); axes.move(to: point(-limit,0)); axes.addLine(to: point(limit,0)); axes.move(to: point(0,-limit)); axes.addLine(to: point(0,limit))
            context.stroke(axes, with: .color(.secondary.opacity(0.5)), lineWidth: 1)
            for (star,x,y) in entries {
                let p = point(x,y), r = star.id == selected ? 4.0 : 2.5
                context.fill(Path(ellipseIn: CGRect(x: p.x-r, y: p.y-r, width: 2*r, height: 2*r)), with: .color(star.id == selected ? .primary : spectralStarColor(star)))
            }
            context.draw(Text("ln(R/G)").font(.caption2), at: CGPoint(x: rect.maxX, y: size.height-5), anchor: .bottomTrailing)
            context.draw(Text("ln(B/G)").font(.caption2), at: CGPoint(x: 0, y: 0), anchor: .topLeading)
            context.draw(Text(String(format:"±%.3f",limit)).font(.caption2).foregroundColor(.secondary), at: CGPoint(x: 0,y: size.height-5), anchor: .bottomLeading)
        }.accessibilityLabel("Red and blue color residuals relative to the selected spectral reference")
    }
}
