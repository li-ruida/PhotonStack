# 背景采样与模型检查

在后期调整 → 高级 → 背景校正中选择“背景采样与模型”。适合线性 FITS 母片，也可检查已有显影图的平滑背景。宽带星系光晕、满幅星云和大范围尘埃都可能被错误地当作背景；采样点需要根据目标人工检查。

## 操作

1. 在原图中查看初始网格。点击空白位置添加采样点，点击已有点可修改位置、半径或禁用；选择“删除”后点击采样点，或使用“删除此点”。
2. 把采样框移出星云、星系光晕和亮星。默认半径为 4 个原图像素；可设置为 1–64。人工采样框必须完全位于图像中，每个像素覆盖率至少为 0.99，否则报告将其标为边缘或覆盖不足。
3. 选择 1–4 阶多项式、校正强度和是否保留背景中值，点击“更新预览”。每通道分别拟合，使用局部中值与迭代残差剔除；至少需要 12 个可用点，且不少于多项式系数个数的两倍。共线或不能约束模型的点位会被拒绝。高阶模型需要更多、分布更均匀的真实背景点。
4. 检查原图、校正图、背景模型及扣除量。原图与校正图使用同一显示拉伸；背景模型单独归一化；扣除量以显示中灰表示零，亮色表示扣除，暗色表示加回。显示拉伸不写入科学 FITS。
5. 点击“保存并使用校正结果”，选择输出父目录。新子目录保存校正图、模型、差图、测量报告和配方，主画布转到校正后的 FITS，可继续后期。

绿色采样点表示三个通道都参与最终拟合，橙色表示部分通道被剔除，红色表示全部被剔除，灰色表示禁用或采样框不合格。选中点可检查每通道的测量值、模型值和采用情况。RMS 是已采用采样点的拟合残差，不能单独证明背景模型没有扣除目标信号。

修改采样点或参数后必须重新预览，才能保存结果。配方包含源文件路径和 SHA-256，重开时会检查源图是否相同。原图位置改变需重新选择源图并建立配方；不自动把其他同名文件当成原图。

## 输出与复验

校正使用线性浮点运算：

~~~
corrected = source - strength * (model - pedestal)
~~~

启用“保留背景中值”时，pedestal 为模型在可测量采样点位置的中值；关闭时为零。差图是实际 source − corrected。保存时不裁去负值或大于一的数值，保留原图覆盖率。对于能识别的 ICRS TAN / TAN-SIP 解算，原图与输出使用相同像素网格并保留投影及正向畸变；报告的 wcsPreserved 表明是否成功保留。未识别的坐标约定不会被冒充为有效解算。

~~~sh
photonstack background-review \
  --input master.fits --output-dir new-review \
  --samples sky-points.txt --degree 2 --strength 0.35 \
  --preserve-brightness on
~~~

采样文件格式如下，坐标以解码后图像左上角像素为 (0, 0)，与预览定位一致；不是 FITS 头的一基 CRPIX 坐标。最后一列是启用状态：

~~~
PhotonStack-BackgroundSamples-1
100 100 8 1
500 100 8 1
300 250 8 0
~~~

此片段仅说明格式，实际拟合需要更多分布均匀的点。省略 --samples 时生成 12×8 网格；--inspect-only on 只生成原图预览与候选点报告。--expected-sha 可指定来源摘要。目标目录必须不存在。

CLI 输出 corrected.fits、model.fits、difference.fits、report.json 和仅供检查的缩略 TIFF。App 导出保存科学 FITS、报告和可重开的 recipe.json；临时缩略图会清理，重复预览只保留当前结果。

- 常规复验：ctest --test-dir build/release -R photonstack_background_review --output-on-failure。
- 使用 NumPy / Astropy 独立核对像素、采样和坐标：[复验脚本](../tools/scripts/qa/background_review_validation.py)。
- 向真实 FITS 注入已知渐变并检查恢复误差：[注入脚本](../tools/scripts/qa/background_gradient_injection.py)。这个测试验证加入的梯度恢复，不把真实星云背景当作已知真值。

## RBF 背景模型

模型选择器支持多项式和 RBF。RBF 使用带一次多项式项的薄板样条，可拟合更局部的平滑渐变；需要 12–512 个有效、非共线采样点。重复像素位置会被拒绝，不会自动丢弃超出上限的采样点。

平滑参数范围为 0–1，默认 0.001；坐标按图像最长边归一化。参数越小，模型越贴合采样，0 表示插值，因此必须避免把星云或星系结构当作背景。数值定义参见 [SciPy RBFInterpolator](https://docs.scipy.org/doc/scipy/reference/generated/scipy.interpolate.RBFInterpolator.html)。

RBF 默认启用留出残差异常点剔除。留出残差来自暂时移除该点后对其位置的预测，区别于拟合残差；它能提示污染采样，但不能证明目标结构受到保护。模型、校正图和差图仍需人工检查。

~~~sh
photonstack background-review \
  --input master.fits --output-dir new-rbf-review \
  --samples sky-points.txt --model rbf --rbf-smoothing 0.001 \
  --rbf-reject on --strength 0.35 --preserve-brightness on
~~~

新保存的配方使用版本 2，明确记录模型和平滑参数；旧版多项式配方可读取并保持原参数。RBF 对每个输出像素直接求值，使用 CPU 并行处理。独立数值参考和异常输入检查可运行 `ctest --test-dir build/release -R photonstack_background_rbf --output-on-failure`。

拼接接缝两侧的独立亮度报告，以及自动识别不可采样区域仍待补齐。
