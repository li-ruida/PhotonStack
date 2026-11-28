import AppKit
import SwiftUI

struct ImagePluginControl:View {
    @ObservedObject var model:PhotonStackWorkspaceModel
    @ObservedObject var session:ImagePluginSession
    private var zh:Bool {model.language == .simplifiedChinese}
    var body:some View {
        VStack(alignment:.leading,spacing:10) {
            Text(zh ? "本地插件":"Local plugins").font(.headline)
            Text(zh ? "选择插件配置和线性 FITS，运行后另存结果。":"Extensions use mono or RGB linear FITS, preserving the grid, units and valid coverage. Loading a definition only inspects it; Run executes the local program.").font(.caption).foregroundStyle(.secondary)
            Button(zh ? "选择插件配置…":"Choose plugin…") {
                let panel=NSOpenPanel();panel.allowsMultipleSelection=false
                guard panel.runModal() == .OK,let url=panel.url else{return}
                Task{await model.inspectImagePlugin(session,definition:url)}
            }.disabled(session.inspecting || model.hasActiveProcessing).accessibilityIdentifier("plugin.definition")
            if session.inspecting {ProgressView()}
            if let info=session.info {
                Text(info.name).font(.subheadline.bold())
                Text(info.executable).font(.caption).textSelection(.enabled)
                Text(info.arguments.joined(separator:"\n")).font(.caption.monospaced()).textSelection(.enabled)
                Text(zh ? "执行文件已校验 · 超时 \(info.timeout) 秒":"Executable verified · timeout \(info.timeout) s").font(.caption).foregroundStyle(.secondary)
            }
            Button(zh ? "选择线性 FITS…":"Choose linear FITS…") {
                let panel=NSOpenPanel();panel.allowsMultipleSelection=false
                guard panel.runModal() == .OK else{return};session.input=panel.url
            }.disabled(session.inspecting || model.hasActiveProcessing).accessibilityIdentifier("plugin.source")
            if let input=session.input {Text(input.lastPathComponent).font(.caption).help(input.path)}
            Text(zh ? "插件会运行本机程序，请只使用可信来源。运行后仍需检查画质。":"Run trusted local extensions. Grid and units are declared by the extension; dimensions, WCS, unit labels, coverage and input hashes are checked. These checks do not establish scientific accuracy.").font(.caption2).foregroundStyle(.secondary)
            Button(zh ? "运行并另存…":"Run & save…") {
                let panel=NativeFilePanels.makeBatchOutputDirectoryPanel(message:zh ? "选择扩展结果的存放目录":"Choose a parent folder for extension output")
                guard panel.runModal() == .OK,let parent=panel.url else{return}
                let output=parent.appendingPathComponent("Plugin-"+UUID().uuidString.prefix(8))
                model.startImagePlugin(session,output:output)
            }.disabled(session.info==nil || session.input==nil || session.inspecting || model.hasActiveProcessing).accessibilityIdentifier("plugin.run")
            if let error=session.error {ProcessingErrorNotice(message:error,chinese:zh)}
        }
    }
}
