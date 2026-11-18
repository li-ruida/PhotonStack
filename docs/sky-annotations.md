# 离线天体标注

在“后期调整 → 高级处理 → 天文定位与 WCS”打开“天体标注与星表”。选择已解算的 FITS 母片，生成标注后可按名称、NGC/IC/M 编号、类别和星等筛选；单击目标列表查看坐标、星等通带、尺寸与轮廓来源。100% 查看可滚动画面。筛选或显示设置改变后需更新标注才能导出，避免保存旧结果。

支持线性单色/RGB/RGBA 和显式 `SRGB-DISPLAY` 的 FITS；拜耳原片须先完成校准与堆栈。输入需要完整且通过几何校验的 ICRS TAN/TAN-SIP WCS。没有坐标时先使用[本地天文定位](local-plate-solving.md)。标注不使用星云的颜色或形状猜测身份，不修改原图，不进行测光或 SPCC。

## 数据与边界

App 安装包内附 [OpenNGC](https://github.com/mattiaverga/OpenNGC/tree/be3cd6c5498806a2fbb2f7e0452ef802dbb02be0)，固定版本 `be3cd6c5498806a2fbb2f7e0452ef802dbb02be0`。CSV、轮廓与数据说明采用 CC BY-SA 4.0，随包保留作者、许可和来源摘要。包含主表与 addendum；排除重复、非存在、单星、双星、新星和无坐标记录后，可查找 12578 个深空对象。公开轮廓取标准亮度 level 2 和无分级轮廓，保留各顶点；转换脚本在 `tools/scripts/import-openngc.py`。

- 原表坐标声明为 J2000。标注按发布值投到 ICRS 图像坐标；不将缺少更详细参考系定义的星云中心当作高精度天体测量参考，也不据投影的数值一致性宣称绝对观测精度。
- 有公开轮廓时优先使用；部分轮廓描述复合星云，会覆盖相邻条目。无轮廓时，仅在长短轴和方向角足够时绘制示意椭圆；圆形不需要方向角。缺失方向不猜测为零度。
- 椭圆轴长使用角直径，位置角从北向东计量。`IC2105` 的 359° 归一到椭圆等价方向 179°；`NGC6857` 的 32767 被标为无效，忽略该角度。原 CSV 保持不变。`IC3322A` 的 `+07:12:60.0` 按秒进位计算。
- 标注是星表尺寸/公开轮廓示意，不是从当前照片测出的真实边界。星等优先 V、缺少时使用 B，详情明确显示通带；无星等条目是否参与筛选由用户选择。
- 边缘目标按中心、轮廓交线或包围关系纳入。轮廓只裁切原有线段，不沿照片边缘补出封闭边界。它描述 WCS 画幅，未按科学图的覆盖掩膜再裁剪。
- 目标按 Messier 优先、已知星等、亮度、编号稳定排列；默认最多 200，可调到 1000，报告保留限制前匹配数量。标签避让采用确定布局；放不下的名称隐藏，目标仍在列表和报告中。

## 输出与重开

“导出标注与配方”在选择的父目录中新建 `SkyAnnotation-…`：

| 文件 | 内容 |
| --- | --- |
| `annotated.svg` | 内嵌 PNG 的独立矢量标注图，可在支持 SVG 的浏览器中查看 |
| `overlay.svg` | 透明标注层与来源页脚，与预览采用相同坐标 |
| `preview.png` | 不含标记的显示预览，默认长边不超过 1600 像素、不放大 |
| `report.json` | 原图零基像素坐标、裁切路径、标签布局、查询和目标详情 |
| `recipe.txt` | 原片摘要、参数及全部产品/星表摘要 |
| `catalog/` | 此次使用的离线星表快照、许可证、作者和上游来源 |

线性母片仅为预览做显示拉伸，关闭统计星色平衡；显示域 FITS 保持已有色彩。预览只供展示，不能作为科学母片。标签布局采用实际缩放尺寸与像素中心映射，App 和 SVG 使用相同报告；App 的列表选择高亮仅用于审查，不写入导出图。

重开、重跑、导出都会验证原片及快照/产品摘要。已有输出目录不覆盖；输入有变化时拒绝继续。导出的 SVG 可独立查看；重新运行配方仍需原路径的科学母片。重跑使用已保存星表快照，不因 App 日后更新星表而更换目标坐标。

```sh
photonstack sky-annotate --input solved.fits --output-dir annotations \
  --category nebulae --limiting-magnitude 18 --include-unknown 1 \
  --max-objects 200 --outlines 1 --labels 1 --preview-width 1600
photonstack sky-annotation-inspect --recipe annotations/recipe.txt
photonstack sky-annotation-replay --recipe annotations/recipe.txt --output-dir annotations-replayed
photonstack sky-annotation-export --recipe annotations/recipe.txt --output-dir annotations-exported
```

App 内置 CLI 优先读取包内 `Resources/OpenNGC`；开发构建使用源码数据目录，独立 CLI 可通过 `--catalog-dir` 指定。性能与独立数值验收见[验证记录](advanced-imaging-validation.zh-CN.md)。
