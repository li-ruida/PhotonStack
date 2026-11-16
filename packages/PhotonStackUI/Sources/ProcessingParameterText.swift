import PhotonStackAppCore

extension AppLanguage {
    func parameterTitle(_ key: String, kind: ProcessingOperationKind) -> String {
        if kind == .photoAdjust, let control = PhotoAdjustments.Control(rawValue: key) {
            return control.title(chinese: self == .simplifiedChinese)
        }
        let zh = self == .simplifiedChinese
        switch key {
        case "width": return text(.previewWidth)
        case "rawWhiteBalance": return text(.rawWhiteBalance)
        case "rawExposureBias": return text(.rawExposureBias)
        case "rawBlackLevel": return text(.rawBlackLevel)
        case "rawDemosaic": return text(.rawDemosaicQuality)
        case "rawLinear": return text(.rawLinearOutput)
        case "darkBiasState": return text(.darkBiasStateLabel)
        case "flatBiasState": return text(.flatBiasStateLabel)
        case "stackMethod": return text(.stackLabel)
        case "method": return text(.stackLabel)
        case "alignment": return text(.alignLabel)
        case "scale": return text(.drizzleScale)
        case "pixfrac": return text(.drizzlePixfrac)
        case "preserveBrightness": return text(.preserveBackgroundBrightness)
        case "protectBrightTargets": return text(.protectBrightBackgroundTargets)
        case "model": return text(.backgroundModel)
        case "targetBackground": return text(.targetBackground)
        case "targetScale": return text(.targetScale)
        case "whitePoint": return text(.whitePoint)
        case "black": return text(.blackPoint)
        case "midInput": return text(.midInput)
        case "midOutput": return text(.midOutput)
        case "white": return text(.whitePoint)
        case "saturation": return text(.saturationAmount)
        case "chromaAmount": return text(.chromaDenoiseParameter)
        case "iterations": return text(.deconvolutionIterations)
        case "sigma": return text(.deconvolutionSigma)
        case "sigmaThreshold": return text(.starDetectionThreshold)
        case "minPeak": return text(.starMinimumPeak)
        case "largeRadius": return text(.largeStarMaskRadius)
        case "layered": return text(.layeredStarMask)
        case "profileAware": return text(.profileAwareStars)
        case "eccentricity": return text(.comaEccentricity)
        case "overlapPixels": return text(.overlapPixels)
        case "projection": return text(.mosaicProjection)
        case "layout": return text(.mosaicLayout)
        case "blend": return text(.mosaicBlend)
        case "exposureMatching": return text(.mosaicExposureMatch)
        case "columns": return text(.mosaicColumns)
        case "amount": return zh ? "强度" : "Amount"
        case "strength": return zh ? "强度" : "Strength"
        case "radius": return zh ? "半径（像素）" : "Radius (px)"
        case "mode": return zh ? "处理模式" : "Mode"
        case "masterMethod": return zh ? "主校准帧合成方法" : "Calibration combine method"
        case "aspect": return zh ? "裁切比例" : "Aspect ratio"
        case "margin": return zh ? "裁边比例" : "Crop margin"
        case "linearDisplayColor": return zh ? "线性光色彩" : "Linear-light color"
        case "stellarBalance": return zh ? "按星点平衡色彩" : "Star color balance"
        case "toneCurve": return zh ? "明暗曲线" : "Tone curve"
        case "toneScale": return zh ? "拉伸尺度" : "Stretch scale"
        case "brightness": return zh ? "亮度" : "Brightness"
        case "background": return zh ? "背景亮度" : "Background brightness"
        case "starExposure": return zh ? "星点曝光" : "Star exposure"
        case "starPeakThreshold": return zh ? "星点峰值阈值" : "Star peak threshold"
        case "starColorCleanup": return zh ? "星点色彩校正" : "Star color cleanup"
        case "redGain": return zh ? "红通道增益" : "Red gain"
        case "blueGain": return zh ? "蓝通道增益" : "Blue gain"
        case "channel": return zh ? "曲线通道" : "Curve channel"
        case "points": return zh ? "曲线控制点" : "Curve points"
        case "fineRadius": return zh ? "细节半径（像素）" : "Fine radius (px)"
        case "starChroma": return zh ? "星点色度" : "Star chroma"
        case "backgroundLimit": return zh ? "背景亮度上限" : "Background limit"
        case "greenMethod": return zh ? "绿色校正方法" : "Green correction method"
        case "greenThreshold": return zh ? "绿色阈值" : "Green threshold"
        case "preserveLightness": return zh ? "保留明度" : "Preserve lightness"
        case "quarterTurns": return zh ? "旋转（90° 步进）" : "Rotation (90° steps)"
        case "cropScale": return zh ? "裁切缩放" : "Crop zoom"
        case "mirror": return zh ? "水平翻转" : "Flip horizontally"
        case "horizontalPosition": return zh ? "水平取景" : "Horizontal framing"
        case "verticalPosition": return zh ? "垂直取景" : "Vertical framing"
        case "maxStars": return zh ? "最多星点数" : "Maximum stars"
        case "edgeAware": return zh ? "优先处理边缘" : "Prioritize edges"
        case "outputFormat": return zh ? "输出格式" : "Output format"
        case "minWeight": return zh ? "最低权重" : "Minimum weight"
        case "recurrenceThreshold": return zh ? "重复出现阈值" : "Recurrence threshold"
        case "removeAirplanes": return zh ? "去除飞机航迹" : "Remove aircraft trails"
        case "removeDrones": return zh ? "去除无人机航迹" : "Remove drone trails"
        case "removeSatellites": return zh ? "去除卫星轨迹" : "Remove satellite trails"
        case "removeMeteors": return zh ? "去除流星" : "Remove meteors"
        default: return key
        }
    }

    func parameterOption(_ value: String, key: String, kind: ProcessingOperationKind) -> String {
        switch key {
        case "alignment":
            if kind == .mosaic { return MosaicAlignmentMode(rawValue: value).map(mosaicAlignmentName) ?? value }
            return AlignmentMethod(rawValue: value).map(alignmentMethodName) ?? value
        case "projection":
            return MosaicProjection(rawValue: value).map(mosaicProjectionName) ?? value
        case "layout":
            return MosaicLayoutMode(rawValue: value).map(mosaicLayoutName) ?? value
        case "blend":
            return MosaicBlendMode(rawValue: value).map(mosaicBlendModeName) ?? value
        case "masterMethod", "stackMethod", "method":
            return StackMethod(rawValue: value).map(stackMethodName) ?? value
        case "darkBiasState", "flatBiasState":
            return CalibrationBiasState(rawValue: value).map(calibrationBiasStateName) ?? value
        case "rawWhiteBalance":
            return RawWhiteBalanceMode(rawValue: value).map(rawWhiteBalanceModeName) ?? value
        case "rawBlackLevel":
            return RawBlackLevelMode(rawValue: value).map(rawBlackLevelModeName) ?? value
        case "rawDemosaic":
            return RawDemosaicQuality(rawValue: value).map(rawDemosaicQualityName) ?? value
        case "channel":
            return CurveChannel(rawValue: value).map(curveChannelName) ?? value
        case "outputFormat": return value.uppercased()
        case "aspect": return value == "original" ? (self == .simplifiedChinese ? "原始比例" : "Original") : value
        case "model", "mode", "toneCurve", "greenMethod":
            switch value {
            case "global": return text(.globalModel)
            case "grid": return text(.gridModel)
            case "subtract": return text(.subtractMode)
            case "divide": return text(.divideMode)
            case "astro-v1": return self == .simplifiedChinese ? "深空显影" : "Deep-sky development"
            case "asinh": return self == .simplifiedChinese ? "反双曲正弦" : "Asinh"
            case "rational": return self == .simplifiedChinese ? "有理曲线" : "Rational curve"
            case "background": return self == .simplifiedChinese ? "背景加权" : "Background-weighted"
            case "average-neutral": return self == .simplifiedChinese ? "平均中和" : "Average neutral"
            default: return value
            }
        default: return value
        }
    }

    func deepSkyChoiceTitle(_ choice: String, field: DeepSkySchema.Field) -> String {
        let chinese = self == .simplifiedChinese
        if field.key == "stack.method", let method = StackMethod(rawValue: choice) {
            return stackMethodName(method)
        }
        if field.key == "registration.mode", let method = AlignmentMethod(rawValue: choice) {
            return alignmentMethodName(method)
        }
        if field.key == "denoise.noise-model" {
            return choice == "scene" ? (chinese ? "从图像估计" : "From image")
                : (chinese ? "从独立子堆栈估计（实验）" : "Independent sub-stacks (experimental)")
        }
        let names: [String: (String, String)] = [
            "none": ("关闭", "Off"), "polynomial": ("多项式", "Polynomial"),
            "grid": ("网格", "Grid"), "polynomial-grid": ("多项式 + 网格", "Polynomial + grid"),
            "asinh": ("反双曲正弦", "Asinh"), "rational": ("有理曲线", "Rational"),
            "bilinear": ("双线性", "Bilinear"), "bicubic": ("双三次", "Bicubic"),
            "malvar-guarded": ("Malvar · 亮星暗环保护", "Malvar · bright-star ringing guard"),
            "linear-light": ("线性光（默认）", "Linear light (default)"),
            "legacy-code": ("旧版显影（兼容重放）", "Legacy rendering (replay)"),
            "malvar": ("Malvar", "Malvar"), "menon": ("Menon", "Menon"),
            "ratio": ("通道比例", "Channel ratio"),
            "median": ("中位数", "Median"), "average": ("平均值", "Average"),
            "unknown": ("未知", "Unknown"), "included": ("包含偏置", "Bias included"),
            "removed": ("已减去偏置", "Bias removed"),
            "camera": ("相机设置", "Camera"), "auto": ("自动", "Automatic"),
            "manual": ("手动", "Manual"), "daylight": ("日光", "Daylight"),
            "fast": ("快速", "Fast"), "balanced": ("均衡", "Balanced"), "high": ("高质量", "High quality")
        ]
        guard let name = names[choice] else { return choice }
        return chinese ? name.0 : name.1
    }
}
