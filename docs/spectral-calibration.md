# 光谱颜色校准：底层接口与实物验证

当前为实验性引擎接口，尚未提供 App 或正式 CLI 的 SPCC 工作流。已实现光谱积分、原生像素孔径测光、候选审查、留出星验证和线性增益应用。设备响应库导入、星表下载、人工审查界面、配方重开及窄带通带参数仍待接入；不能将本阶段称为完整 SPCC。

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

Python 需 NumPy、SciPy 和 Astropy；输入 CSV 的列为 `source_id,ra,dec,pmra,pmdec,phot_g_mean_mag`，XP 缓存是 ESA DataLink `DATAMODEL_GAIA` CSV 的 `xp-*.download` 文件。输出包含原生报告、独立复核报告、输入 SHA-256、实验夹具及通过检查时的线性 `calibrated.fits`。输入保持只读，拒绝已有输出目录。正式星表获取、响应导入和来源绑定配方仍是后续工作。
