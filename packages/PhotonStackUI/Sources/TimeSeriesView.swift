import AppKit
import SwiftUI
import Charts
import UniformTypeIdentifiers

struct TimeSeriesView:View {
    @ObservedObject var model:PhotonStackWorkspaceModel
    @ObservedObject var session:TimeSeriesSession
    @Environment(\.dismiss) private var dismiss
    @State private var loadingTask:Task<Void,Never>?
    private var zh:Bool {model.language == .simplifiedChinese}
    private var busy:Bool {loadingTask != nil || session.busy || model.hasActiveProcessing}
    var body:some View {
        VStack(alignment:.leading,spacing:12) {
            HStack {
                Text(zh ? "时间序列测光":"Time-series photometry").font(.headline)
                Spacer()
                Button(zh ? "打开结果…":"Open result…") {openResult()}.disabled(busy).accessibilityIdentifier("photometry.open")
                ToolSheetCloseButton(chinese: zh, busy: busy) { dismiss() }
            }
            HStack(alignment:.top,spacing:18) {
                VStack(alignment:.leading,spacing:10) {
                    if let report=session.report {results(report)} else {canvas}
                    Text(zh ? "先点目标星，再点至少两颗比较星。点击星名可重新选位置。":"Click the target, then at least two comparison stars. Select an existing star row to reposition it on the image. Coordinates are zero-based from the top left.").font(.caption).foregroundStyle(.secondary)
                    starList
                }.frame(maxWidth:.infinity,maxHeight:.infinity)
                ScrollView {controls.frame(maxWidth:.infinity,alignment:.leading)}.frame(width:300)
            }
            if let error=session.error {ProcessingErrorNotice(message:error,chinese:zh)}
            if !busy, session.report == nil, let reason=session.configuration.validationMessage(chinese:zh) {
                Text(reason).font(.caption).foregroundStyle(.secondary).accessibilityIdentifier("photometry.requirement")
            }
            ToolActionBar {
                if busy {
                    ToolTaskStatus(title: zh ? "正在处理…" : "Processing…", chinese: zh, cancelIdentifier: "photometry.cancel") {
                        if let loadingTask {loadingTask.cancel()} else {model.cancelCurrentTask()}
                    }
                }
                Spacer()
                if let directory=session.directory {Button(zh ? (session.report == nil ? "上次结果":"查看结果文件") : (session.report == nil ? "Previous result":"Show result files")){NSWorkspace.shared.activateFileViewerSelecting([directory])}}
                Button(zh ? "测量并保存…":"Measure & save…"){run()}.toolPrimaryAction().disabled(!session.configuration.isValid || busy).accessibilityIdentifier("photometry.run")
            }
        }.padding(20).frame(width:1040,height:720).interactiveDismissDisabled(busy).onDisappear{loadingTask?.cancel()}
    }
    private var controls:some View {
        VStack(alignment:.leading,spacing:12) {
            Button(zh ? "选择 FITS 序列…":"Choose FITS sequence…"){chooseFrames()}.disabled(busy).accessibilityIdentifier("photometry.frames")
            Text(zh ? "\(session.configuration.frames.count) 帧 · 固定比较星集合":"\(session.configuration.frames.count) frames · fixed comparison ensemble").font(.caption)
            Text(zh ? "请选择同滤镜、已校准并对齐的线性 FITS。彩色原片需先去马赛克。":"Use same-filter linear images on a common grid. Calibrate and demosaic CFA frames first. This tool does not align or calibrate them.").font(.caption).foregroundStyle(.secondary)
            Toggle(zh ? "确认相同网格和滤镜":"Confirm common grid and filter",isOn:$session.configuration.aligned).toggleStyle(.checkbox).accessibilityIdentifier("photometry.aligned")
            Picker(zh ? "通道":"Channel",selection:$session.configuration.channel){Text(zh ? "单色":"Mono").tag("mono");Text("R").tag("red");Text("G").tag("green");Text("B").tag("blue")}.accessibilityIdentifier("photometry.channel")
            HStack {Text(zh ? "滤镜名称":"Filter name");TextField(zh ? "滤镜名称" : "Filter name",text:$session.configuration.filter).textFieldStyle(.roundedBorder).accessibilityIdentifier("photometry.filter")}
            field(zh ? "有效增益 e⁻/单位":"Effective gain e⁻/unit",$session.configuration.gain,"gain")
            Text(zh ? "填写每单位像素值对应的电子数，不是 ISO 或相机增益档位。":"Use effective electrons per current sample unit, not ISO or a camera gain setting. Stacking or scaling changes this value.").font(.caption2).foregroundStyle(.secondary)
            field(zh ? "孔径半径 px":"Aperture radius px",$session.configuration.radius,"radius")
            field(zh ? "背景内半径 px":"Sky inner radius px",$session.configuration.skyInner,"inner")
            field(zh ? "背景外半径 px":"Sky outer radius px",$session.configuration.skyOuter,"outer")
            field(zh ? "饱和值（0 未知）":"Saturation (0 unknown)",$session.configuration.saturation,"saturation")
            field(zh ? "背景信噪比下限":"Minimum background SNR",$session.configuration.minSNR,"snr")
            field(zh ? "最大质心偏移 px":"Maximum center shift px",$session.configuration.maxShift,"shift")
            Text(zh ? "使用曝光中点时间（JD UTC）。缺少或冲突的时间会标为无效。":"Times use DATE-OBS/MJD-OBS plus half the exposure, reported as JD UTC. Missing or inconsistent times are flagged. CLI recipes can supply explicit UTC starts and exposures.").font(.caption2).foregroundStyle(.secondary)
            DisclosureGroup(zh ? "测量说明" : "Measurement details") {
                Text(zh ? "误差估计不包括大气闪烁、平场误差、重采样相关性和归一化误差。比较星稳定度采用留一法计算。堆栈或缩放图像后，须重新换算有效增益。" : "Uncertainties exclude scintillation, flat-field errors, resampling covariance and normalization errors. Comparison stability uses leave-one-out RMS. Recalculate effective gain after stacking or scaling.").font(.caption).foregroundStyle(.secondary)
            }
            Text(zh ? "显示相对亮度及噪声估计，不是绝对星等。":"Errors estimate background and source photon noise, excluding scintillation, flat-field errors, resampling covariance and normalization uncertainty. This is not absolute or standard-filter photometry.").font(.caption2).foregroundStyle(.secondary)
        }.disabled(busy)
    }
    private func field(_ label:String,_ value:Binding<Double>,_ id:String)->some View {
        HStack {Text(label).font(.caption);Spacer();TextField(label,value:value,format:.number).frame(width:85).textFieldStyle(.roundedBorder).accessibilityIdentifier("photometry."+id)}
    }
    private var canvas:some View {
        GeometryReader {geometry in
            if let image=session.image,session.width>0,session.height>0 {
                let ratio=Double(session.width)/Double(session.height)
                let width=min(geometry.size.width,geometry.size.height*ratio)
                let height=width/ratio
                ZStack {
                    Image(nsImage:image).resizable()
                    Canvas {context,size in
                        for (i,star) in session.configuration.stars.enumerated() {
                            let center=CGPoint(x:(star.x+0.5)/Double(session.width)*size.width,y:(star.y+0.5)/Double(session.height)*size.height)
                            let radius=max(5,session.configuration.radius*size.width/Double(session.width))
                            context.stroke(Path(ellipseIn:CGRect(x:center.x-radius,y:center.y-radius,width:2*radius,height:2*radius)),with:.color(i==0 ? .orange:.cyan),lineWidth:session.selection==i ? 3:1.5)
                            context.draw(Text(star.id).font(.caption).foregroundColor(i==0 ? .orange:.cyan),at:CGPoint(x:center.x,y:center.y-radius-9))
                        }
                    }.allowsHitTesting(false)
                }.frame(width:width,height:height).contentShape(Rectangle()).onTapGesture {point in
                    guard !busy else{return};session.tap(x:point.x/width*Double(session.width)-0.5,y:point.y/height*Double(session.height)-0.5)
                }.frame(maxWidth:.infinity,maxHeight:.infinity)
            } else {ContentUnavailableView(zh ? "选择线性 FITS 序列":"Choose a linear FITS sequence",systemImage:"star.circle")}
        }.background(.black).accessibilityIdentifier("photometry.canvas")
    }
    private var starList:some View {
        ScrollView {
            VStack(alignment:.leading,spacing:5) {
                ForEach(Array(session.configuration.stars.enumerated()),id:\.element.id) {index,star in
                    HStack {
                        Button(star.id){session.selection=index;session.report=nil}.buttonStyle(.bordered).tint(index==0 ? .orange:.cyan)
                        Text(String(format:"x %.2f · y %.2f",star.x,star.y)).font(.caption.monospacedDigit())
                        Spacer()
                        Button{session.configuration.stars.remove(at:index);session.selection=nil;if index==0, !session.configuration.stars.isEmpty {session.configuration.stars[0].id="target"}}label:{Image(systemName:"minus.circle")}.buttonStyle(.borderless)
                    }.accessibilityIdentifier("photometry.star.\(star.id)")
                }
            }
        }.frame(height:100).disabled(busy)
    }
    private func results(_ report:TimeSeriesReport)->some View {
        VStack(alignment:.leading,spacing:8) {
            Text(zh ? "有效 \(report.accepted) / \(report.frames) 帧":"Accepted \(report.accepted) / \(report.frames) frames").font(.headline)
            let points=report.points.filter{$0.status=="measured"}.sorted{($0.jdUTC ?? 0)<($1.jdUTC ?? 0)}
            let start=points.first?.jdUTC ?? 0
            if points.isEmpty {ContentUnavailableView(zh ? "没有有效测量":"No accepted measurements",systemImage:"exclamationmark.triangle")}
            else {
                Chart(points) {point in
                    let minutes=((point.jdUTC ?? start)-start)*1440
                    let value=point.relativeFlux ?? 0
                    let error=point.relativeError ?? 0
                    RuleMark(x:.value("Minutes",minutes),yStart:.value("Lower",value-error),yEnd:.value("Upper",value+error)).foregroundStyle(.secondary)
                    PointMark(x:.value("Minutes",minutes),y:.value("Relative flux",value))
                }.chartXAxisLabel(zh ? "距首个有效中点（分钟）":"Minutes from first accepted midpoint").chartYAxisLabel(zh ? "相对通量":"Relative flux").chartYScale(domain:.automatic(includesZero:false)).frame(minHeight:180).accessibilityIdentifier("photometry.chart")
            }
            Text(zh ? "比较星亮度波动（越小越稳定）":"Comparison leave-one-out relative RMS (review required)").font(.caption.bold())
            ForEach(report.comparisons){comparison in Text(comparison.id+" · "+(comparison.leaveOneOutRelativeRMS.map{String(format:"%.4f%%",$0*100)} ?? "—")).font(.caption.monospacedDigit())}
            if !report.saturationKnown {Text(zh ? "未填写饱和值，请检查亮星是否过曝。":"Saturation threshold unknown; only obvious plateaus checked.").font(.caption).foregroundStyle(.orange)}
            ScrollView {
                LazyVStack(alignment:.leading,spacing:4) {
                    ForEach(report.points.filter{$0.status != "measured"}) {point in Text("#\(point.index+1) · "+point.detail).font(.caption).foregroundStyle(.orange)}
                }
            }.frame(maxHeight:100)
        }.frame(maxWidth:.infinity,maxHeight:.infinity,alignment:.topLeading)
    }
    private func chooseFrames(){
        let panel=NSOpenPanel();panel.allowsMultipleSelection=true;panel.allowedContentTypes=["fits","fit","fts"].compactMap{UTType(filenameExtension:$0)}
        guard panel.runModal() == .OK else{return};let urls=panel.urls
        loadingTask=Task{defer{loadingTask=nil};await model.chooseTimeSeriesFrames(session,frames:urls)}
    }
    private func run(){
        let panel=NativeFilePanels.makeBatchOutputDirectoryPanel(message:zh ? "选择光变曲线与测量记录的存放目录":"Choose a parent folder for light curves and measurements")
        guard panel.runModal() == .OK,let parent=panel.url else{return}
        model.startTimeSeries(session,output:parent.appendingPathComponent("Photometry-"+UUID().uuidString.prefix(8)))
    }
    private func openResult(){
        let panel=NativeFilePanels.makeImportFolderPanel(message:zh ? "选择含 report.json 的测光结果目录":"Choose a photometry result folder containing report.json")
        guard panel.runModal() == .OK,let url=panel.url else{return}
        loadingTask=Task{
            defer{loadingTask=nil}
            do{try await model.openTimeSeries(session,output:url)}catch is CancellationError {}catch{session.error=error.localizedDescription}
        }
    }
}
