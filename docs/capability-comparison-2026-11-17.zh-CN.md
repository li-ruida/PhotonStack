# PhotonStack 0.1.5 与 PixInsight、Siril 的深空处理能力对照

核查日期为 2026-11-17。PhotonStack 对应提交 `d2dd227`，对照 PixInsight 1.9.6 build 1718 和 Siril 1.4.5。PhotonStack 依据发布版 CLI、App 工作流、引擎和现存 FITS；竞品依据官方发布说明和稳定版手册。本机 Siril 为 1.4.2。本调查比较能完成的操作，没有用三款软件重算同一组原片，不能据此给画质、速度或成功率排名。[PixInsight 开发团队公告](https://pixinsight.net/dev/index.php) · [Siril 1.4.5 发布说明](https://siril.org/download/2026-10-28-siril-1-4-5/)

## 哪些拍摄任务还需要切换软件

| 任务 | PixInsight 的参照能力 | Siril 的参照能力 | PhotonStack 0.1.5 的实际边界 |
|---|---|---|---|
| 找目标、天球配准、拼接 | 1.9.6 提供全平台盲解，可在不知道中心坐标时求解星场 | 可利用 WCS/SIP 解算、校正畸变，把小重叠面板投到共同画布，也可用 Astrometry.net 盲解 | 已有按子目录分区、各区堆栈、共同星点匹配、曝光匹配、羽化和多频段融合。未找到从星表求解、使用已有 WCS 放置天区并写回成片坐标的工作流。小重叠时缺少另一条可靠的定位路径。[PI 公告](https://pixinsight.net/dev/index.php) · [Siril 拼接实操](https://siril.org/tutorials/mosaics/) |
| OSC 双窄带、单色机多滤镜 | ChannelCombination、LRGBCombination、PixelMath 支持按通道构造结果 | 直接从拜耳数据提取 Hα/OⅢ，可分别堆栈、对齐、匹配强度，再组合 RGB/LRGB 或使用表达式 | RGB 显影、逐通道增益、曲线和图层能调成片。缺少从双窄带原片到两张线性母片、再到可复现 HOO 的流程，也缺少多张单色母片的通道映射与像素表达式。[PI 通道组合源码](https://gitlab.com/pixinsight/PCL/-/blob/master/src/modules/processes/ColorSpaces/ChannelCombinationProcess.h) · [Siril 通道提取](https://siril.readthedocs.io/en/stable/processing/extraction.html) · [Siril 合色](https://siril.org/tutorials/rgb_composition/) |
| 跨夜恒星颜色一致 | SPCC 使用恒星光谱和系统响应作颜色参考 | SPCC 提供 Gaia DR3、相机和滤镜响应，也有按中心波长与带宽设置的窄带模式 | App 有统计星色平衡及白平衡取样，CLI 的 color-calibrate 可用参考图的匹配恒星估计相对校正。未找到使用星表光谱的 PCC/SPCC、仪器响应库和拟合质量审查。宽带星色与窄带艺术配色应有不同入口。[Siril SPCC 手册](https://siril.readthedocs.io/en/stable/processing/color-calibration/spcc.html) · [PhotonStack 参考图标定](../engine/include/photonstack/StarColorCalibrator.hpp) |
| 满幅星云的背景与接缝 | 除传统背景提取外，MGC 可使用全天参考数据 MARS | 多项式和径向基函数（RBF）背景模型，支持检查、添加、移除采样点；拼接另有重叠区归一化 | 已有多项式、网格、星云保护区和分区曝光匹配。最缺的是让用户检查哪些位置被当成背景、剔除污染样本、查看将被扣掉的模型，并量化重叠区校正前后的差异。[PI MARS 公告](https://pixinsight.net/dev/index.php?page=1) · [Siril 背景提取](https://siril.readthedocs.io/en/stable/processing/background.html) |
| 单独拉星云、控制恒星、再合成 | 可用掩膜和 PixelMath 组合图像，去星另有第三方工具 | 能调用外部 StarNet，再独立拉伸星层和无星层、重新合成 | 有星点检测、蒙版、缩星、星色修整和画笔。缺少从当前图像成对生成无星层与可回加星层、分开处理、重新合成的用户流程。外部去星工具的授权和安装条件应单列。[Siril StarNet 与重组](https://siril.readthedocs.io/en/stable/processing/stars/starnet.html) |
| 明暗拉伸、细节与降噪 | 有多尺度自适应拉伸 MAS，已有原生机器学习降噪 MLDenoise | 有广义双曲拉伸，可从实测星点估计点扩散函数（PSF）后反卷积 | 已有 asinh、曲线、局部对比度、多尺度降噪、非局部降噪及 Richardson–Lucy 反卷积。后者使用设定的高斯核；CLI 另有 PSF 匹配。值得补的是从未饱和星点估计 PSF、预览亮星振铃和保护暗部结构。不同算法的实际成片收益须同图测量。[PI 开发公告](https://pixinsight.net/dev/index.php) · [Siril 反卷积](https://siril.readthedocs.io/en/stable/processing/deconvolution.html) · [现有反卷积](../engine/src/Deconvolution.cpp) |
| 跨夜、多设备、多滤镜预处理 | WBPP 提供分组预处理和中断恢复 | 脚本、序列和不同校准组可组合处理，支持暗场缩放与坏像素图 | 已有暗偏平校准、质量筛片、异常像素剔除、批处理和配方复用。当前以目标目录及其校准子目录为主，缺少按滤镜、曝光、增益、温度、binning 等匹配校准库、处理前展示匹配理由和冲突的统一入口。[PI WBPP 公告](https://pixinsight.net/dev/index.php?page=1) · [Siril 1.4 能力说明](https://siril.org/download/2025-12-05-siril-1-4-0/) · [PhotonStack 工作流](reusable-workflows.md) |
| 欠采样彩色相机的 Drizzle | 支持 DrizzleIntegration | 支持直接使用未去马赛克的 CFA 数据做彩色 Drizzle | 有普通 Drizzle。按拜耳像素原色累加、逐通道计算覆盖率的 CFA Drizzle 已有实验引擎，尚未接到普通 App/CLI Drizzle。适合有帧间位移、覆盖足够的数据，单纯放大画布不能验证分辨率收益。[Siril 拼接与 RGB Drizzle](https://siril.org/tutorials/mosaics/) · [PhotonStack 实现边界](cfa-drizzle.md) |
| 交换文件、扩展与测量 | XISF、JavaScript、可扩展处理模块 | FITS/XISF、Python、脚本库、孔径测光、比较星、光变与 AAVSO 导出 | 已有线性 FITS、显示 TIFF、CLI、可复用处理链。未找到 XISF 交换、第三方处理模块接口、时间序列差分测光；SER 行星视频也不在当前公开流程内。按深空成片定位，这些可以分别排期。[PI 开发与 XISF 公告](https://pixinsight.net/dev/index.php?page=1) · [Siril 发布说明](https://siril.org/download/2026-10-28-siril-1-4-5/) · [PhotonStack 格式边界](features.md) |

## 面纱星云实物核查

读取了项目中两张参考母片和两个已生成拼接结果的 FITS 主头。两张 2160 × 3840 参考母片均带有 TAN-SIP 天球投影及二阶畸变参数。T1 的参考点为赤经 313.690517°、赤纬 +29.793685°，T2 为 312.138026°、+30.932312°。现存 3913 × 3905 和 3917 × 3907 的两张拼接 FITS 都缺少 CTYPE1、CRVAL1、CRVAL2。这是历史成品的实测；当前 0.1.5 接口和引擎也未找到对应的 WCS 拼接输出路径。[拼接接口](../engine/include/photonstack/MosaicBuilder.hpp) · [拼接实现](../engine/src/MosaicBuilder.cpp)

这意味着照片仍可依靠共同星点成功拼接，但其他天文软件打开上述成品时无法直接从头信息读出像素对应的赤经、赤纬。WCS 应在裁切、缩放和重投影后重新计算，直接复制 T1 的头会写入错误坐标。已有坐标可提供面板初始位置；实际重叠星点仍适合检验残差，亮度接缝则需单独处理。

## 按现有素材建议的开发顺序

1. **先让双窄带得到可编辑的 Hα 与 OⅢ 两张线性母片。** 在导入时选择滤镜类型，按实际拜耳排列和两条通带估计信号，各自保留覆盖和强度。合色面板提供 HOO 起点（红用 Hα，绿、蓝用 OⅢ），允许调整混合比例并保存配方。用面纱原片与 Siril 在同尺度逐通道对比背景、未饱和星点和细丝；验证重开后映射相同。如果只拍了 Hα/OⅢ，两条通带无法提供独立实测的 SⅡ。
2. **为已有解算的面板接入 WCS，再加求解器。** 先支持读取、投影、检验和写出，已有坐标的面纱母片可以直接验收。用独立软件打开拼接 FITS，检查中心、四角和接缝的同名恒星，报告中位及高分位残差。再覆盖没有 WCS、旋转、小重叠和错误头信息的输入。星表解算也为后续目标标注和 SPCC 提供基础。
3. **把背景模型和拼接接缝展示出来。** 支持调整样本、预览背景模型及校正前后差值；用面纱检验细丝是否被扣除，用 M31 检验外围光晕。同步看重叠区通道差和接缝两侧背景，避免仅靠压暗背景掩盖问题。
4. **后期增加星层独立处理，宽带增加星表标色。** 星云拉伸与恒星亮度可分别调节；先验证不调整时重组能还原输入，再检查亮星暗环及弱星损失。用宽带 M31 验证 SPCC，保留匹配星数、剔除原因与色差散布；用窄带面纱验证独立的滤镜参数和调色路径。
5. **随后补多夜素材管理和 CFA Drizzle。** 前者用两夜、两滤镜、混合曝光和一组不匹配平场验证分组与冲突提示。后者用有抖动位移的欠采样序列检查每色覆盖、星点形状、噪声与有限值，再决定是否纳入一键成片。

验收应同时保留线性母片和显影成片。功能调用成功、星点变锐或图像更鲜艳，分别回答不同问题；不足以单独证明测光、空间分辨率或颜色准确度有所提高。
