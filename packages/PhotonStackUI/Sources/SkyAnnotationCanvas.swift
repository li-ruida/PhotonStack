import AppKit
import SwiftUI

struct SkyAnnotationCanvas: View {
    let report: SkyAnnotationReport
    let image: NSImage
    let selected: String?
    private func color(_ object: SkyAnnotationReport.Object) -> Color {
        switch object.category {
        case "galaxies": Color(red: 1, green: 205/255.0, blue: 120/255.0)
        case "clusters": Color(red: 157/255.0, green: 189/255.0, blue: 1)
        default: Color(red: 100/255.0, green: 228/255.0, blue: 213/255.0)
        }
    }
    var body: some View {
        Canvas { context, size in
            let pw = Double(report.previewWidth), ph = Double(report.previewHeight)
            let sx = pw/Double(report.width), sy = ph/Double(report.height)
            context.scaleBy(x: size.width/pw, y: size.height/(ph+40))
            context.fill(Path(CGRect(x: 0, y: 0, width: pw, height: ph+40)), with: .color(Color(red: 8/255.0, green: 12/255.0, blue: 16/255.0)))
            context.draw(Image(nsImage: image), in: CGRect(x: 0, y: 0, width: pw, height: ph))
            for o in report.objects {
                let ink = color(o), selected = selected == o.id
                if report.showOutlines {
                    for points in o.paths {
                        var path = Path()
                        for (i, p) in points.enumerated() {
                            let point = CGPoint(x: (p[0]+0.5)*sx, y: (p[1]+0.5)*sy)
                            if i == 0 { path.move(to: point) } else { path.addLine(to: point) }
                        }
                        context.stroke(path, with: .color(ink), lineWidth: selected ? 2.5 : 1.2)
                    }
                }
                if o.centerVisible {
                    let x = (o.x+0.5)*sx, y = (o.y+0.5)*sy
                    context.stroke(Path(ellipseIn: CGRect(x: x-4, y: y-4, width: 8, height: 8)), with: .color(ink), lineWidth: selected ? 2.5 : 1.2)
                }
                if report.showLabels && o.labelVisible {
                    var link = Path(); link.move(to: CGPoint(x: o.anchorX, y: o.anchorY)); link.addLine(to: CGPoint(x: o.labelX, y: o.labelY+10))
                    context.stroke(link, with: .color(ink.opacity(0.6)), lineWidth: 1.2)
                }
            }
            if report.showLabels {
                for o in report.objects where o.labelVisible {
                    let rect = CGRect(x: o.labelX, y: o.labelY, width: o.labelWidth, height: 20)
                    context.fill(Path(roundedRect: rect, cornerRadius: 3), with: .color(Color.black.opacity(0.85)))
                    if selected == o.id { context.stroke(Path(roundedRect: rect, cornerRadius: 3), with: .color(color(o)), lineWidth: 1) }
                    context.draw(Text(o.label).font(.system(size: 13, design: .monospaced)).foregroundColor(color(o)), at: CGPoint(x: o.labelX+5, y: o.labelY+3), anchor: .topLeading)
                }
            }
            context.draw(Text("OpenNGC · CC BY-SA 4.0").font(.system(size: 11)).foregroundColor(.white.opacity(0.82)), at: CGPoint(x: 8, y: ph+4), anchor: .topLeading)
            context.draw(Text("Approximate catalog outlines").font(.system(size: 11)).foregroundColor(.white.opacity(0.65)), at: CGPoint(x: 8, y: ph+19), anchor: .topLeading)
        }.accessibilityLabel("Deep-sky object annotation preview")
    }
}
