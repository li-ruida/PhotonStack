# 使用实测 PSF 恢复线性通道

在后期调整 → 高级 → 反卷积 → 从恒星测量 PSF 中，打开或生成测量，然后展开「使用实测 PSF 恢复通道」。恢复仅使用该测量对应的原母片及通道；修改检测参数或排除项后，须先另存测量。RGB 可以分别测量、恢复 R/G/B，再通过合色入口组合。亮度测量生成单色亮度结果，不会把同一个亮度核默默套到三个颜色通道。

设置迭代次数、混合强度、负值补偿、噪声抑制和亮星保护阈值，选择「恢复并另存」。原始素材只读，结果目录必须新建。输出可重开恢复设置，也可检查各产品：

| 文件 | 含义 |
|---|---|
| `channel.fits` | 实际参与恢复的 Float32 原通道，用于严格前后对照 |
| `restored.fits` | 恢复后再按强度和保护范围混合的线性结果 |
| `difference.fits` | 恢复结果减原通道，保留正负值 |
| `residual.fits` | 原通道减恢复结果再卷积，保留正负值 |
| `protection.fits` | 0–1 亮星保护权重；缺失处为 NaN |
| `recipe.txt` | 测量来源摘要、选项、统计及五个产品的 SHA-256 |

通道图使用三个相同颜色平面及覆盖平面保存单色信号；保护图为单平面。有效覆盖和原母片的 TAN/SIP WCS 保留。预览自动拉伸，不能用不同预览的视觉亮度直接比较通量；原通道、恢复图和差图保留物理像素数值。

## 数值模型

令 `H` 为零延拓的有限域 PSF 卷积，`c` 为有效覆盖，`d = Hc`。正向算子为 `A(x) = H(cx)/d`。权重为 `c` 的 Richardson–Lucy 更新使用真正的转置卷积及灵敏度归一化：

```
x <- x * Hᵀ(c * ratio / d) / Hᵀ(c / d)
ratio = observed / A(x)                 # 不启用噪声抑制时
```

边缘与洞口不环绕、不复制边缘像素。FFT 画布至少为图幅加核尺寸减一，再扩展到 2/3/5 因子尺寸，使用 Double、CPU pocketfft；线程数默认 4。不是 GPU 实现。当前限制为 16 megapixels、奇数且不超过 63×63 的非负核、1–100 次迭代。测量保存的是全局椭圆高斯核；空间变化 PSF、Moffat/ePSF 仍不在此入口中。[pocketfft 接口](https://github.com/mreineck/pocketfft/blob/cpp/README.md)

- **负值补偿**：在恢复前明确加上指定常数，恢复后减回。原像素加补偿仍有负值会停止；不会自动压黑或估计补偿。这改变了迭代所用信号的噪声解释，不能把它当作完整泊松/读出噪声建模。
- **噪声抑制**：残差为 `r` 时，把比率偏离 1 的幅度乘以 `r²/(r²+σ²)`。σ 使用母片像素单位，0 关闭。它减弱接近噪声尺度的更新，是启发式阻尼，不是自动降噪、方差估计或收敛保证。
- **亮星保护**：以原通道达到阈值的像素为核心，向外扩展一个核半径完全保留原通道，再在第二个核半径内余弦过渡。距离为棋盘距离，0 阈值关闭。它保留原始亮核及邻域，不能恢复已剪切的恒星通量，也不会自动识别所有亮星光晕。
- **混合强度**：最后执行 `原通道 + 强度 × (1−保护权重) × 恢复变化`。全保护处与强度 0 处原值精确保留。报告的残差和通量基于最终混合结果。

RL 会放大噪声，迭代更多并不保证更真实；阻尼和局部保护也不能保证全局测光守恒。需检查弱结构、孔径通量、亮星暗环和空间变化警告。[RL 噪声与迭代限制研究](https://arxiv.org/html/2411.00991v1)

## 命令行与复验

```sh
photonstack psf-restore --measurement /path/measurement/recipe.txt \
  --output-dir /path/new-restoration --iterations 8 --amount 0.7 \
  --pedestal 0 --noise-sigma 5 --protect-above 8000 --threads 4
photonstack psf-restoration-inspect --recipe /path/new-restoration/recipe.txt
photonstack psf-restoration-replay --recipe /path/new-restoration/recipe.txt \
  --output-dir /path/new-replay
```

数值示例仅用于说明单位，须按素材调整。检查和重跑会校验测量配方、原母片与 PSF 产品；打开结果还校验所有恢复产品。源文件或依赖被修改时拒绝继续。测量目录需与结果一同保留。

`tools/scripts/qa/psf_restoration_validation.py` 用独立 SciPy 线性卷积复算全图迭代、覆盖、保护与产品；`psf_restoration_quality.py` 用已知星点、微弱结构、噪声与截断亮星的合成场检查噪声和暗环。两者均使用 NumPy、SciPy、Astropy，要求新的输出目录。验证记录见[进阶能力验收](advanced-imaging-validation.zh-CN.md)。
