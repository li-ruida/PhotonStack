import AppKit
import SwiftUI
import UniformTypeIdentifiers

struct XisfExchangeView:View {
    @ObservedObject var model:PhotonStackWorkspaceModel
    @Environment(\.dismiss) private var dismiss
    @State private var importing=true
    @State private var input:URL?
    @State private var image=0
    @State private var compression="zlib"
    private var zh:Bool {model.language == .simplifiedChinese}
    private var options:XisfExchangeOptions? {
        input.map { XisfExchangeOptions(input:$0,importing:importing,image:image,compression:compression) }
    }
    var body:some View {
        VStack(alignment:.leading,spacing:14) {
            Text(zh ? "XISF 文件交换":"XISF exchange").font(.headline)
            Picker(zh ? "方向":"Direction",selection:$importing) {
                Text(zh ? "导入 XISF → FITS":"Import XISF → FITS").tag(true)
                Text(zh ? "导出 FITS → XISF":"Export FITS → XISF").tag(false)
            }.pickerStyle(.segmented).accessibilityIdentifier("xisf.direction").onChange(of:importing){input=nil;image=0}
            Text(zh ? "转换 XISF 与 FITS，保留像素值和位深，不改原文件。":"Preserve sample values and precision without display stretching. Supports mono, RGB and Bayer; source files are retained.").font(.callout).foregroundStyle(.secondary)
            Button(zh ? "选择文件…":"Choose file…") {
                let panel=NSOpenPanel();panel.allowsMultipleSelection=false
                panel.allowedContentTypes=(importing ? ["xisf"] : ["fits","fit","fts"]).compactMap{UTType(filenameExtension:$0)}
                guard panel.runModal() == .OK else{return};input=panel.url
            }.accessibilityIdentifier("xisf.source")
            if let input {Text(input.path).font(.caption).lineLimit(2).textSelection(.enabled)}
            if importing {
                HStack {Text(zh ? "图像序号（从 0 开始）":"Image index (zero-based)");TextField("0",value:$image,format:.number).frame(width:70).textFieldStyle(.roundedBorder).accessibilityIdentifier("xisf.image")}
                Text(zh ? "多图文件可按序号选择。转换后自动导入项目，完整 XML 信息另存。":"Select one image from a multi-image file. The full XML is retained; FITSKeyword entries and 2×2 Bayer patterns map into FITS.").font(.caption).foregroundStyle(.secondary)
            } else {
                Picker(zh ? "压缩":"Compression",selection:$compression){Text("zlib").tag("zlib");Text(zh ? "无损未压缩":"Uncompressed").tag("none")}.accessibilityIdentifier("xisf.compression")
                Text(zh ? "仅导出 FITS 主图像，不包含扩展数据。遇到不支持的编码会提示原因。":"Export the primary image on its existing grid. Scaled integers, Bayer offsets and bottom-up orientation require explicit conversion first; extension HDUs are not exported.").font(.caption).foregroundStyle(.secondary)
            }
            if importing, !(0..<1024).contains(image) {
                Text(zh ? "图像序号须为 0–1023。" : "Image index must be 0–1023.").font(.caption).foregroundStyle(.orange)
            }
            ToolActionBar {
                Spacer()
                Button(zh ? "取消":"Cancel"){dismiss()}.keyboardShortcut(.cancelAction)
                Button(zh ? "转换并保存…":"Convert & save…") {
                    guard let options,options.isValid else{return}
                    let panel=NativeFilePanels.makeBatchOutputDirectoryPanel(message:zh ? "选择转换结果的存放目录":"Choose a parent folder for the converted image")
                    guard panel.runModal() == .OK,let parent=panel.url else{return}
                    let output=parent.appendingPathComponent("XISF-"+UUID().uuidString.prefix(8))
                    model.startXisfExchange(options,output:output);dismiss()
                }.toolPrimaryAction().disabled(options?.isValid != true || model.hasActiveProcessing).accessibilityIdentifier("xisf.convert")
            }
        }.padding(24).frame(width:580)
    }
}
