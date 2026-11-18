# 恒星 PSF 测量与审查

在后期调整 → 高级 → 反卷积中展开「从恒星测量 PSF」，选择单色或 RGB 线性 FITS 母片。检测阈值默认 5σ；饱和值使用母片像素单位，0 表示未知，只能排除明显的平台饱和，并不保证所有饱和星均被识别。

测量会另建结果目录，保存单位和的 `psf.fits`、仅用于显示的 `stars-preview.tiff` 和包含来源及产品 SHA-256 的 `recipe.txt`。原母片保持只读。打开测量会重新验证文件；星点审查支持手动排除并另存重测。改变测量通道或阈值后须重新测量，再编辑对应候选星，避免沿用旧编号。

## 模型与范围

- 最多审查最亮的 2048 个候选星，在原始分辨率的 15×15 邻域拟合椭圆高斯与局部平面背景。无完整覆盖、边缘、饱和、拥挤、拟合失败与宽度异常会分别标记；拥挤判断限于候选列表，不能保证排除所有暗弱邻星。
- 至少需要 12 颗合格星，覆盖至少四个 4×4 分区，且横纵跨度均达到图幅的 40%。合格星协方差等权平均，生成截断到最大轴标准差四倍半径的归一化高斯核。
- 报告包含长短轴 FWHM、角度、星宽分位数和 4×4 区域统计。角度从图像 +X 指向存储行 +Y，不是天球位置角。空间变化警告是启发式检查，没有警告也不能证明全图 PSF 一致。
- 本模型是点采样高斯，不是像素积分 ePSF、Moffat 或 Airy 模型。小于两像素的星点可能受采样相位影响；拟合宽度不等于光学分辨率。

**当前保存与审查测量核；下方手动高斯反卷积尚未自动使用此核。** 实测核恢复、局部变化和噪声/亮星保护仍需后续实现及验证。

## 命令行

```sh
photonstack psf-measure --input /path/linear.fits --output-dir /path/new-measurement \
  --channel luminance --sigma 5 --saturation 0
photonstack psf-inspect --recipe /path/new-measurement/recipe.txt
# 根据该测量的候选编号另存重测
photonstack psf-measure --input /path/linear.fits --output-dir /path/new-exclusion \
  --exclude 6
```

通道可选 `luminance`、`red`、`green`、`blue`。CFA 原始图和显式非线性 FITS 会被拒绝；未知编码的 FITS 需由调用者确认其为线性数据。输出目录必须不存在。配方保存每颗星的拟合邻域锚点、协方差、状态及区域统计，打开时校验来源与两个产品的摘要。

独立复核脚本 `tools/scripts/qa/psf_measurement_validation.py` 使用 NumPy、Astropy 和 SciPy，从源像素重新拟合选定恒星并检查核及报告。独立优化器的一致性验证实现正确性，不证明真实星翼符合高斯模型。
