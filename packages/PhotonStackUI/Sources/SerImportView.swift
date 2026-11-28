import AppKit
import SwiftUI

struct SerImportView:View {
    @ObservedObject var model:PhotonStackWorkspaceModel
    @Environment(\.dismiss) private var dismiss
    @State private var input:URL?
    @State private var info:SerSequenceInfo?
    @State private var first=0
    @State private var count=200
    @State private var step=1
    @State private var byteOrder="compatibility"
    @State private var error:String?
    @State private var inspecting=false
    private var zh:Bool {model.language == .simplifiedChinese}
    private var options:SerImportOptions? {
        guard let input,let info else{return nil}
        let value=SerImportOptions(input:input,first:first,count:count,step:step,byteOrder:byteOrder)
        return value.isValid(total:info.frames) ? value:nil
    }
    var body:some View {
        VStack(alignment:.leading,spacing:14) {
            Text(zh ? "导入 SER 视频":"Import SER video").font(.headline)
            Text(zh ? "将视频逐帧转为 FITS，并加入当前项目。原视频不变。":"Extract FITS frames into the current project, retaining pixel codes, Bayer pattern and available per-frame UTC times. The original video is preserved.").font(.callout).foregroundStyle(.secondary)
            Button(zh ? "选择 SER 文件…":"Choose SER…") {
                let panel=NSOpenPanel();panel.allowsMultipleSelection=false
                guard panel.runModal() == .OK,let url=panel.url else{return}
                input=url;info=nil;error=nil;first=0
            }.accessibilityIdentifier("ser.source")
            if let input {Text(input.path).font(.caption).lineLimit(2).textSelection(.enabled)}
            Picker(zh ? "像素字节序":"Pixel byte order",selection:$byteOrder) {
                Text(zh ? "兼容 Siril / SER Player":"Siril / SER Player compatible").tag("compatibility")
                Text(zh ? "按原始规范":"Original specification").tag("specification")
                Text("Little endian").tag("little");Text("Big endian").tag("big")
            }.accessibilityIdentifier("ser.byte-order")
            if inspecting {ProgressView()}
            if let info {
                Text("\(info.width) × \(info.height) · \(info.frames) " + (zh ? "帧":"frames") + " · \(info.depth) bit · \(info.bayer.isEmpty ? (info.channels==1 ? "Mono":"RGB"):info.bayer)")
                Text(info.timestamps ? (zh ? "含逐帧时间表":"Per-frame timestamp table available"):(zh ? "无逐帧时间表；不会推测帧时间":"No per-frame timestamps; frame times will not be inferred")).font(.caption).foregroundStyle(.secondary)
                HStack {
                    field(zh ? "首帧（从 0 开始）":"First frame (zero-based)",value:$first,id:"first")
                    field(zh ? "抽取数量":"Count",value:$count,id:"count")
                    field(zh ? "间隔":"Step",value:$step,id:"step")
                }
                Text(zh ? "每次最多导入 10000 帧。导入后可校准、筛片和堆栈。":"Up to 10000 frames per import. Review, select and stack them in the workspace. Extraction does not perform sensor calibration.").font(.caption).foregroundStyle(.secondary)
            }
            if let error {ProcessingErrorNotice(message:error,chinese:zh)}
            ToolActionBar {
                Spacer()
                Button(zh ? "取消":"Cancel") {dismiss()}.keyboardShortcut(.cancelAction)
                Button(zh ? "导出并导入…":"Extract & import…") {
                    guard let options else{return}
                    let panel=NativeFilePanels.makeBatchOutputDirectoryPanel(message:zh ? "选择存放 FITS 序列的目录":"Choose a parent folder for the FITS sequence")
                    guard panel.runModal() == .OK,let parent=panel.url else{return}
                    let output=parent.appendingPathComponent("SER-"+UUID().uuidString.prefix(8))
                    model.startSERImport(options,output:output);dismiss()
                }.disabled(options==nil || inspecting || model.hasActiveProcessing).toolPrimaryAction().accessibilityIdentifier("ser.import")
            }
        }.padding(24).frame(width:580)
        .task(id:(input?.path ?? "")+":"+byteOrder) {
            guard let input else{return};info=nil;inspecting=true;defer{inspecting=false}
            do {
                let value=try await model.inspectSER(input,byteOrder:byteOrder)
                try Task.checkCancellation();info=value;count=min(max(1,count),min(10000,value.frames));error=nil
            }catch is CancellationError {} catch {if !Task.isCancelled {self.error=error.localizedDescription}}
        }
    }
    private func field(_ label:String,value:Binding<Int>,id:String)->some View {
        VStack(alignment:.leading){Text(label).font(.caption);TextField(label,value:value,format:.number).textFieldStyle(.roundedBorder).accessibilityIdentifier("ser."+id)}
    }
}
