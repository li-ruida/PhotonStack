# 光谱颜色校准：离线审查与实物验证

当前提供 App 离线审查与 CLI，可导入外部响应与缓存光谱，执行候选审查、保存完整报告和科学母片，并重开、重算及导出。已有光谱积分、原生像素孔径测光、留出星验证和线性增益应用。内置设备响应库管理、在线星表获取及独立窄带通带参数仍待接入；不能将本阶段称为完整 SPCC。

## App 审查流程

在后期调整的「高级 → 色彩校正」打开「光谱星色校准（SPCC）」。选择带 ICRS WCS 的宽带线性 RGB FITS 母片，以及按下节导入得到的 `profile.txt`、`catalog.txt`，执行「校准并审查」。如果当前画布是已显影图，需在窗口中另选原线性母片。

- 前后图使用共同显示尺度。黄色为拟合星、青色为独立留出星、橙色为剔除星；点击星点或选择列表可查看信噪比、颜色残差、扩大孔径差与剔除原因。画幅外候选仍保留在列表中。
- 可按 ID 搜索、按采用/剔除筛选，并手动排除候选。排除或孔径设置改变后，旧结果仍可比较，但必须重新审查才能保存。更换星表会清除旧排除项。
- 对数残差图的横轴是 R/G、纵轴是 B/G；两种比较状态共用尺度。它包含相对白参考的归一化，不把白参考的颜色与仪器增益混为一谈。近似响应会明确提示；通过留出门限不证明整机响应准确。
- 「保存审查与配方」将快照、报告和预览存入新目录；验收失败时同样可以保存诊断，但不生成校准母片。「另存并使用校准母片」只在当前结果通过验收时可用，主画布打开全尺寸线性 `calibrated.fits`。
- 主画布识别母片旁的 SPCC 配方，经来源和产品摘要复核后，按同一显示参数生成全尺寸 TIFF，保留 1:1 查看所需的像素尺寸，避免通用拉伸错误处理已缩放的背景基线。显示图不进入编辑图；后续科学操作仍读取线性 FITS。单独导出的 FITS 若没有旁边的配方目录，则由查看软件自行决定显示拉伸。
- 「打开审查结果」选择之前保存的目录，恢复来源、快照、孔径、完整质量门限和手动排除。保存时重新验证来源与产品摘要、按快照复算，不能覆盖已有结果；运行中出现的同名文件或目录也保留。关闭窗口会取消待完成任务，临时预览在任务结束后清理。

## 离线使用

使用明确带 ICRS WCS 的原生 RGB/RGBA 线性 FITS。单色、CFA、显示域、缺少 WCS 的输入会被拒绝。设备与滤镜响应由使用者选定，不从文件名推测。`tools/scripts/import-spcc-data.py` 可将外部 OSC 响应 JSON、CALSPEC 白参考和 ESA Gaia DR3 XP sampled 缓存转换为输入：

```sh
python3 tools/scripts/import-spcc-data.py \
  --sensor /data/Sony_IMX585.json --response-license-file /data/LICENSE.md \
  --ideal-cutoff 400 700 \
  --white-calspec /data/sun_reference_stis_002.fits \
  --white-name 'CALSPEC Sun' \
  --white-source https://ssb.stsci.edu/trds/calspec/sun_reference_stis_002.fits \
  --white-license 'STScI CALSPEC public calibration reference' \
  --profile-name 'IMX585 with assumed 400-700 nm IRCUT' \
  --gaia-catalog /data/m31-gaia.csv --xp-dir /data/xp-cache \
  --observation-date 2026-08-18T20:04:18.382445 --output-dir /data/new-reference-import

photonstack spcc --input /data/solved-linear-rgb.fits \
  --profile /data/new-reference-import/profile.txt \
  --catalog /data/new-reference-import/catalog.txt \
  --output-dir /data/new-calibration
```

示例的 400–700 nm 是明确的近似条件，不是该设备实测通带。若已有共同 OSC 滤镜透过率 JSON，使用 `--filter /data/filter.json` 代替 `--ideal-cutoff`。当前导入器将组件曲线组合标为 `approximate`，不将曲线来源等级自动等同于实测整机精度；它不是独立的窄带/HOO 标色入口。

导入脚本需 NumPy、Astropy；原生 `spcc` 不依赖 Python。导入会校验波长、单位、响应范围、光谱误差、唯一 ID，以及 XP 光谱与 Gaia 行的原始坐标一致性。缺失光谱或自行的星记录在 `import.json`，不默认为零自行。以归一化位置与切向速度传播至指定历元，未虚构距离、径向速度或视差。保留输入摘要、来源元数据和 `response-source-license.txt`，外部数据不装入应用资源。

可用参数包括 `--radius 8 --sky-inner 11 --sky-outer 15 --min-snr 20 --saturation 0 --exclude source-id,...`。饱和阈值 0 表示未知；排除 ID 必须存在且唯一。`--expected-sha` 可将操作绑定到已经审查过的原片摘要。

质量门限可用 `--max-center-offset 2 --max-spectral-error 0.1 --max-aperture-color-change 0.03 --max-validation-log-error 0.2` 指定，分别约束质心位移（像素）、光谱相对误差上界、扩大孔径对数颜色变化及留出对数残差 P90。完整设置随配方保存，在 App 重开、重新审查时保留；改变门限会改变通过条件，不能将放宽门限视为精度提升。

结果目录包含：

| 文件 | 内容 |
|---|---|
| `profile.txt`、`catalog.txt` | 实际使用的响应、白参考、已传播星位及光谱快照 |
| `report.json` | 配置、来源、全部候选及剔除原因、两孔径测量、拟合/留出残差、仪器与白参考增益 |
| `before.tiff` | 16 位显示预览 |
| `after.tiff`、`calibrated.fits` | 仅通过留出验收时生成；分别为显示预览和保留 WCS/覆盖的 Float32 线性科学母片 |
| `recipe.txt` | 来源、参数、依赖与产品 SHA-256；报告同时绑定此配置 |

质量未通过的运行仍保存可检查的报告；`report.fit.valid=false` 时没有校准母片，并禁止导出。CLI 成功退出表示审查已完成，不等于颜色校准通过。

```sh
photonstack spcc-inspect --recipe /data/new-calibration/recipe.txt
photonstack spcc-replay --recipe /data/new-calibration/recipe.txt --output-dir /data/new-replay
photonstack spcc-export --recipe /data/new-calibration/recipe.txt --output /data/new-export.fits
photonstack spcc-preview --recipe /data/new-calibration/recipe.txt --output /data/new-preview.tiff
photonstack spcc-preview --recipe /data/new-calibration/recipe.txt --output /data/new-full-display.tiff --full-resolution 1
```

重开、重算与导出均检查原片、依赖快照和产品摘要。修改配方参数但保留旧报告也会被拒绝。重算使用保存的设置重新运行，并要求产品与旧配方一致；若后续算法改变结果，应建立一次新的审查。已有目录与文件不覆盖，失败时清理本次创建的未完成输出。

`spcc-preview` 默认复制通过验收且摘要正确的显示 TIFF；`--full-resolution 1` 先重现原图显示预览并核对其摘要，确认当前显示算法与保存结果一致，再用同一黑场参考和尺度渲染全尺寸校准母片。两种模式都不生成科学数据，同样拒绝已有目标、被修改的依赖和未通过的结果。App 的缓存区分配方、依赖和显示产品的文件状态。

预览采用覆盖加权的面积缩采样，避免在缩小的星点核心中仅选几个像素。前后使用同一原图统计、同一显示尺度；后图的黑场参考与科学通道增益同步变换。它不是再次自动白平衡，也没有修改科学母片的背景。报告记录显示范围与公式，不能把显示 TIFF 的非线性值当成测光结果。

## 数值定义

- `SpectralCalibration.hpp`：光谱为按波长的能量通量密度 Fλ，波长单位 nm；响应由 1–8 层 0–1 的 QE/透过率曲线相乘。计算 `∫ Fλ(λ) R(λ) λ dλ`，省略各通道共同的 `1/(hc)`。这是相对光子信号，不是绝对电子计数率。负光谱采样值保留，不因噪声而截零。
- 每个响应文件的采样范围之外假定响应为零，因此仪器曲线端点应覆盖实际通带；光谱必须覆盖响应交集，拒绝光谱外推。按全部曲线节点划分区间后使用八点 Gauss–Legendre 积分。误差只给出波长间完全相关假设的保守上界，不冒充由 Gaia 完整协方差得到的标准差。
- `StellarPhotometry.hpp`：在同一拟合中心测量 RGB，默认半径 8 px、背景环 11–15 px，边缘像素按 8×8 子采样加权。各通道用稳健背景平面；背景噪声计入背景平面估计的条件方差，不包含源光子噪声和重采样协方差。显式饱和阈值可选；另拒绝单通道至少五个近等峰值像素形成的平顶亮核。未检出平顶不等于证明未饱和。
- `SpectralCalibrator.hpp`：输入已传播到观测时刻的 ICRS 星位、RGB 响应及白参考光谱。拒绝星表近邻、检测到的近邻、位置不匹配、缺少光谱误差、相对光谱误差上界 >10%、每通道背景信噪比 <20、覆盖缺失及孔径颜色不稳定的候选。以增大 2 px 的孔径及背景环重测，要求 R/G、B/G 的对数颜色变化均 ≤0.03。
- 检测采用完整有效覆盖和全图稳健背景统计、3σ 阈值；检测数超过 100000 时拒绝使用不完整候选集证明孤立性。星表和检测近邻以孔径半径 +4 px 为范围，用空间格子查询。低于检测阈值的邻星、未分辨双星、变星仍可能遗漏。
- 通过上述候选检查后按位置排序，每五颗留出一颗。以拟合星的对数颜色增益中位数及 MAD 拒绝离群值；留出星不再剔除。至少 12 颗拟合星、6 颗留出星、留出星覆盖 4/16 个区域；默认留出 R/G、B/G 绝对对数残差 P90 ≤0.2。该上限是操作验收门限，不是精密测光精度保证。
- 白参考通过相同响应积分。最终增益为仪器增益除以白参考相对 G 的信号，G 固定为 1。应用仅乘增益，保留负值、部分覆盖和线性范围；不拉伸、不扣背景、不截取到 0–1，并拒绝 Float32 溢出。

这些检查不能证明所选设备响应准确，也不能自动消除大气通量、星云背景、通道 PSF 差异或此前颜色矩阵变换的影响。直接在相机 RGB 上的响应模型，不应未经验证用于已改变颜色基底的图像。只有 Hα/OⅢ 的窄带数据不能凭宽带接口获得独立的 SⅡ 或三通道连续谱。

## M31 实物复核

本机原片头记录为 Seestar S30 Pro、IRCUT、GRBG。使用已完成 WCS 解算的 2160×3840 线性 RGB 母片；母片未保留完整拍摄日期，故明确按一张原片的 `2026-08-18T20:04:18.382445` 传播 Gaia DR3 自行，不能认为这就是整组素材的精确平均历元。

参考数据：

- Gaia DR3：以 ICRS (10.69°, 41.276°)、半径 2.4° 查询 `has_xp_sampled = 'True'`、9<G<15 的前 240 颗亮星。两颗缺少自行，实际输入 238 颗。XP sampled 采用 336–1020 nm、2 nm 间距的 343 个样本，包含每波长通量误差。[Gaia DR3 合成测光论文](https://arxiv.org/abs/2206.06215)
- IMX585 三色 QE：外部 Siril 响应数据库提交 `304efb29396e0aaf7a38adb10ef01fc8b236e70d` 的 `Sony_IMX585.json`，质量标记 2，来自厂家图表数字化；该外部数据库采用 GPLv3，仅作为用户侧独立 QA 数据，不复制到本项目资源或安装包。[响应数据源](https://gitlab.com/free-astro/siril-spcc-database)
- 滤镜：暂用 400–700 nm 的理想单位透过率，并受所提供传感器曲线端点限制。**不是 S30 Pro IRCUT 的实测整机响应**，未加入大气模型。
- 白参考：STScI CALSPEC `sun_reference_stis_002.fits`；Å 转 nm，FLAM 转 W m⁻² nm⁻¹ 的因子为 0.01。[CALSPEC 数据说明](https://www.stsci.edu/hst/instrumentation/reference-data-for-calibration-and-tools/astronomical-catalogs/calspec.html)

238 颗中，122 颗在画幅外、3 颗靠近边缘；其余拒绝包括 57 颗检测近邻、2 颗星表近邻、1 颗光谱误差过大、1 颗质心拟合失败、1 颗孔径颜色不稳定。最终 40 颗参与拟合、1 颗拟合离群、10 颗独立留出，覆盖 8/16 个区域。太阳白参考下的增益约为 R=1.06458、G=1、B=0.96914；留出对数残差 P90 分别为 R/G=0.10920、B/G=0.04307，约相当于 11.5% 和 4.4% 的乘性偏差。通过默认门限不代表已达到精密星色标定。

独立 Python 验证采用局部多项式乘积的解析积分，不复用 C++ 的求积算法；用 NumPy 加权最小二乘重算背景平面、孔径通量、条件噪声、全部拟合和留出统计，另用 SciPy 独立拟合 48 颗星质心。最大相对光谱积分差 1.56e−15、孔径通量差 2.45e−14、扩大孔径差 3.09e−14，最大质心差 1.10e−4 px。最终 Float32 科学产品全部像素与独立增益公式完全相等，33×33 WCS 网格、覆盖及来源摘要复核通过。数值一致性不能替代整机响应的物理准确性验证。

一次完整原生探针调用（含原片读取、基础与候选审查两轮测量、积分和科学 FITS 写出）约 0.55 秒，峰值 RSS 386711552 字节。暖文件缓存，旁有 Python 验证活动；不是冷缓存或跨设备基准，不宣称 GPU 加速。

## 复验入口

原生合成测试覆盖已知通道增益、噪声、低单通道信噪比、单通道亮核截断、邻星、重复位置、异常光谱误差、扩大孔径颜色变化、手动排除、无效 WCS、显示域输入、负值和覆盖保持。真实数据探针是显式构建的手动 QA 工具，不加入缺少数据时也会显示通过的自动用例：

```sh
cmake --preset release
cmake --build --preset release --target photonstack_spectral_calibration_probe photonstack_spectral_calibrator_tests
ctest --preset release -R spectral_calibra

python3 tools/scripts/qa/spectral_calibration_validation.py \
  --input /path/to/solved-linear-rgb.fits \
  --catalog /path/to/m31-gaia.csv --xp-dir /path/to/xp-cache \
  --response /path/to/Sony_IMX585.json \
  --white /path/to/sun_reference_stis_002.fits \
  --probe build/release/tests/engine/photonstack_spectral_calibration_probe \
  --observation-date 2026-08-18T20:04:18.382445 \
  --output-dir /path/to/new-audit
```

Python 需 NumPy、SciPy 和 Astropy；输入 CSV 的列为 `source_id,ra,dec,pmra,pmdec,phot_g_mean_mag`，XP 缓存是 ESA DataLink `DATAMODEL_GAIA` CSV 的 `xp-*.download` 文件。输出包含原生报告、独立复核报告、输入 SHA-256、实验夹具及通过检查时的线性 `calibrated.fits`。输入保持只读，拒绝已有输出目录。

`tests/cli/test_spectral_calibration.py` 用不依赖科学 Python 库的解析星场验证正式 CLI，包含所有科学像素、WCS、16 位 TIFF 公式、质量失败、手动排除、快照和产品修改、原片修改、配方参数修改及重算/导出字节一致性。`tests/cli/test_spcc_import.py` 另需 NumPy、Astropy，验证自行跨 RA=0、单位转换、负光谱值、输入只读、重复导入、坐标不匹配和异常误差拒绝。

对上述真实 M31，正式 CLI 科学输出与底层探针逐字节一致，增益与留出统计未改变。`tools/scripts/qa/spcc_cli_validation.py` 独立重算光谱积分、原孔径和扩大孔径测量、拟合与全图增益；源 WCS、重算全部文件和再次导出通过复核。自行传播与 [Astropy 的空间运动接口](https://docs.astropy.org/en/stable/coordinates/apply_space_motion.html) 的最大差约 7.02e−11 角秒；保留其无已知距离提示，不将此结果解释为绝对天体测量精度。面积缩采样及共同显示公式在 17×17 位置复算，前后 TIFF 差分别为 0/1 个 16 位刻度；合成原尺寸 TIFF 全像素检查也通过。

第一次共用固定黑点的显示把母片原有较大背景基线变成明显的通道偏色，实际看图后改为同步变换黑场参考；科学增益公式与 FITS 未改变。另一次独立预览校验发现通用缩放器采用双线性抽样，遂为该比较预览实现面积积分，并保持严格显示误差门限。失败工件保留于本机 QA 目录。

## 文本输入约定

`profile.txt` 首行为 `PhotonStack-SpectralProfile-1`，随后是五个带引号的元数据字段：名称、`broadband`、质量（`measured`/`approximate`/`synthetic`）、来源说明、许可说明。接下来依次为 R/G/B：组件数量（1–8），每组件的采样数量及 `波长nm 响应值` 行。最后是白参考名称/来源/许可三个带引号字段、采样数量及 `波长nm Fλ` 行。所有曲线总样本最多 200000；质量字段是用户声明，验收不能证明其真实性。

`catalog.txt` 首行为 `PhotonStack-SpectralCatalog-1`，随后是名称、已传播星位的十进制儒略年历元、来源说明、许可说明及恒星数量。每颗星为带引号的 ID、ICRS 赤经/赤纬（度）、样本数量，随后是 `波长nm Fλ flux_error` 行。最多 10000 颗星、总计 4000000 个样本；波长严格递增、通量有限、误差非负，ID 唯一。原生接口不再传播一次自行，也不根据图片文件名推测历元。
