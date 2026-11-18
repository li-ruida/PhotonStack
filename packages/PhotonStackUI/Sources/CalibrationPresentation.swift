import Foundation

enum CalibrationPresentation {
    static func role(_ value: String, chinese: Bool) -> String {
        chinese ? ["bias":"偏置", "dark":"暗场", "dark-flat":"暗平场", "flat":"平场"][value] ?? value : value.capitalized
    }
    static func status(_ value: String, chinese: Bool) -> String {
        let zh = ["compatible":"参数匹配", "review":"待核对", "incompatible":"不匹配", "conflict":"冲突", "invalid":"无效", "missing":"缺失",
                  "unique-compatible":"一个兼容候选", "ambiguous":"多个候选待选择", "review-required":"缺少信息，待核对", "no-compatible-master":"无兼容母片", "missing-master":"未添加母片"]
        let en = ["compatible":"Parameters match", "review":"Review", "incompatible":"Incompatible", "unique-compatible":"One compatible candidate", "ambiguous":"Choose between candidates",
                  "review-required":"Missing information", "no-compatible-master":"No compatible master", "missing-master":"No master added"]
        return (chinese ? zh[value] : en[value]) ?? value
    }
    static func field(_ value: String, chinese: Bool) -> String {
        guard chinese else { return value }
        return ["camera":"相机", "cameraSerial":"相机标识", "sampling":"采样排列", "filter":"滤镜", "opticalTrain":"光路标识", "readoutMode":"读出模式",
                "domain":"线性域", "units":"单位", "object":"目标", "observedAt":"拍摄时间", "utcDate":"UTC 日期", "frameType":"帧类型", "timeSystem":"时标",
                "exposure":"曝光 / 秒", "gain":"增益", "iso":"ISO", "gainOrISO":"增益或 ISO", "offset":"偏置设定", "sensorTemperature":"实测温度 / °C", "setTemperature":"设定温度 / °C",
                "binX":"横向 binning", "binY":"纵向 binning", "originX":"传感器起点 X", "originY":"传感器起点 Y", "focalLength":"焦距", "electronsPerADU":"电子 / ADU",
                "width":"宽度", "height":"高度", "channels":"通道数"][value] ?? value
    }
    static let fields = ["object", "observedAt", "camera", "cameraSerial", "filter", "opticalTrain", "sampling", "domain", "units", "readoutMode",
                         "exposure", "gain", "iso", "offset", "sensorTemperature", "setTemperature", "binX", "binY", "originX", "originY", "electronsPerADU"]
    static func value(_ key: String, in source: CalibrationAcquisition, chinese: Bool) -> String {
        if key == "sensorTemperature", let low = source.numeric["sensorTemperatureMin"], let high = source.numeric["sensorTemperatureMax"] {
            let style = FloatingPointFormatStyle<Double>.number.precision(.fractionLength(0...6))
            return low.formatted(style) + "…" + high.formatted(style)
        }
        if let value = source.text[key] { return value }
        if let value = source.numeric[key] { return value.formatted(.number.precision(.fractionLength(0...6))) }
        return chinese ? "未知" : "Unknown"
    }
}
