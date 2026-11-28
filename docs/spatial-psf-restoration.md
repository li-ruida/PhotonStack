# 空间变化 PSF 与噪声抑制

PSF 恢复可以选择整幅统一核，或四象限空间模型。空间模型在每个象限要求至少三颗已接受、未排除的测量星点，以协方差均值建立正定椭圆高斯核；四个核在象限中心之间连续插值，画面边缘使用最近端权重。不满足星点覆盖时停止，不能静默退回统一核。

实现采用源位置权重：`H = Σ K_j W_j`，精确伴随为 `Hᵀ = Σ W_j K_jᵀ`。有效性、边界归一化和伴随灵敏度均进入迭代；不会分块独立处理再硬拼边界。FFT 工作区逐核复用，当前实际后端为 CPU double pocketfft。空间模式增加计算量，不能当成 GPU 加速。

噪声模型控制 Richardson–Lucy 残差抑制，**不是**对 Poisson-Gaussian 混合似然的精确最大似然求解：

- 固定：方差为 `σ²`。
- 光子：`σ² + max(prediction - pedestal, 0) / gain`。
- 光子与读出：再加 `(readNoiseElectrons / gain)²`。

`gain` 为当前源像素单位对应的电子数，不是相机 ISO 标签；归一化、加权平均、重采样后的母片需要相应的有效增益与噪声标定。这里没有建模重采样产生的空间相关噪声。显式 pedestal 用来覆盖负值，迭代后减回；它不计入光子方差。

CLI 示例：

```
photonstack psf-restore --measurement measurement/recipe.txt --output-dir restored \
  --spatial-mode quadrants --noise-model poisson-gaussian \
  --electrons-per-unit 2 --read-noise-electrons 3 --iterations 8
```

这些数值只是参数示例。配方 v2 保留模型与噪声设置，v1 配方继续按统一核和固定噪声重播。结果提供再卷积残差、变化差图和亮星保护图。更低残差或更细星点不能单独证明分辨率真实提升；高斯近似、欠采样和噪声放大仍需检查。参见 [STScI 对图像恢复的说明](https://hst-docs.stsci.edu/drizzpac/chapter-3-description-of-the-drizzle-algorithm/3-1-imagereconstructionandrestorationtechnique)。

本地验证包含独立显式成像矩阵（非对称、不同尺寸的四个核，有限边界、部分覆盖、空洞、正反算子、三种噪声方差）、常量背景不变，以及 CLI 配方检查和逐字节重播。实拍细节真实性需要另行验收。
