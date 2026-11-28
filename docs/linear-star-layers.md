# 线性星点分层

在后期的星点分层工具中可选择线性母片，或导入已经对齐、同尺寸、同像素单位的线性无星 FITS。程序保存原始副本、无星层、有符号星点残差、SHA-256 来源记录及独立显示预览。默认重组复制原始 FITS；调整星点或星云 EV、饱和度后另存 Float32 FITS。负值、有效性与 WCS 保留。Gamma 不在这个线性运算阶段应用；拉伸放在重组之后。

`star-layers linear-split --input original.fits --starless starless.fits --output-dir layers`

两份输入都声明 FITS 像素单位（`BUNIT`）时，分层前会核对单位及大小写。星点层、无星层和调整后重组的 FITS 均沿用原始母片声明的单位。

`star-layers linear-render --layers layers --stars-ev -1 --output-dir result`

## 外部去星工具

线性适配器定义 `linearFITS: true`。它使用最大有效绝对值把三通道 Float32 FITS 缩放至 [-1,1]，仅在模型输入中以背景样本填充无效像素，推理后恢复单位与原始有效性。填充区域附近仍可能影响模型预测，需要人工检查。显示 TIFF 适配器与线性 FITS 适配器不可混用。

StarNet 线性适配器要求用户选择自己安装的支持线性 FITS 的版本，阅读该版本的完整许可并明确接受；接受记录绑定许可 SHA-256，条款变化后需要重新确认。模型及运行库不随 PhotonStack 分发。新版 2.6 的输入约束参考 [StarNet 官方 CLI 文档](https://starnetastro.com/documentation/starnet/command-line-tool/)。

推理结果是估计，不能证明被移除内容全为恒星。残差可能包含星云细丝、光晕和负值；必须查看三层及局部放大。当前本地独立数值和 App 服务测试覆盖算术、单位、掩码、WCS、重播与源文件保护，不代表新版模型的实拍质量已经验收。

## 文件含义

- `original.fits`：原始字节副本。
- `starless.fits` / `stars.fits`：线性 Float32 图层；像素数值恢复原始单位，保留 WCS 和有效性。新写出的图层不复制全部拍摄 FITS 头；完整头仍在原始副本中。
- `render-*/combined.fits`：科学重组数据。`combined.tiff` 为独立显影预览，不应拿来替代线性母片。
- `layers.txt`、`provenance.txt`、`session.json`：图层摘要、来源与配方。外部运行另外保存适配器定义和日志。
