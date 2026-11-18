# 本地天文定位与 WCS

高级工具中的“天文定位与 WCS”支持从星表识别单色或 RGB/RGBA FITS 母片的天区。可忽略缺失、错误的已有坐标，另存带 TAN/SIP WCS 的 FITS，再交给已有 WCS 配准、拼接流程。当前不自动为整个待堆栈序列逐帧求解，也不包含目标标注、星表色彩标定或在线求解服务。

## 准备与操作

需要本机安装 Astrometry.net 的 `solve-field` 和适合视场的索引。macOS 可使用 `brew install astrometry-net`；App 识别 Homebrew 常见安装位置，也允许选择其他本地可执行文件。索引不是 App 安装包的一部分，需要从 [Astrometry.net 官方索引目录](https://data.astrometry.net/)取得。选择覆盖目标天区与尺度的文件，确认索引采用 ICRS 坐标后开始定位。图像和提取星点不会上传。

每组索引覆盖的角尺度不同。例如官方 4108 的星组尺度为 30–42 角分；这不意味着它能解决任意视场。具体选择见[官方说明](https://astrometry.net/doc/readme.html#getting-index-files)。可提供每像素角秒的上下界以减少搜索，位置始终盲搜；不使用原图的 WCS 或中心坐标。默认 CPU 上限与墙钟上限均为 120 秒，可调至 600 秒。

定位完成后显示母片和星点标记：黄色为高置信度拟合匹配，青色为未参与求解的留出星。审查中心坐标、像素尺度、留出残差和区域覆盖，再保存。保存后的 `solved.fits` 在主画布中打开；“打开结果”选择结果目录可恢复报告与参数。修改设置后须重新定位才能保存。

## 数据、精度与失败边界

- 检测并拟合最多 2000 颗候选星；按亮度排列，每第五颗留出，只把其余星的 1-based X/Y/FLUX 表交给求解器。至少需要 40 颗可测星。不从现有 WCS 取提示，也不把留出星传给求解器。
- 拟合表含有低概率候选匹配，固定使用 `match_weight >= 0.9` 的匹配作复核；这不是按残差删到通过。至少 12 星、4/16 区、横纵跨度各 40%、P95 ≤2 px、最大残差 ≤5 px。
- 留出检测与求解器输出的投影索引星作唯一、互为最近邻的 3 px 匹配，并排除半径内歧义。至少 6 星、3/16 区、横纵跨度各 30%、P95 ≤2 px。匹配检验没有再次拟合 WCS。这个统计是同一索引的留出一致性，不能代替独立 Gaia 星表的绝对精度评估。
- 全画幅 17×17 网格检查映射及逆映射；拒绝折叠、相对 TAN 的面积比例小于 0.5 或大于 2、逆映射误差超过 1e-4 px。此界限针对常规深空视场，不保证大视场鱼眼可解。
- Astrometry.net 的[输出实现](https://github.com/dstndstn/astrometry.net/blob/main/util/sip_qfits.c)固定写 `EQUINOX=2000`，即使输入星表采用 ICRS。接口因此要求显式声明索引为 ICRS，再将结果写为 `RADESYS=ICRS`；不猜测其他参考系，也不把 FK5 当作 ICRS。普通 WCS 读取仍保持原有严格要求。
- 新母片只替换主头中的 WCS，去掉冲突的旧 WCS/逆 SIP/查表畸变与失效校验和。BITPIX、BSCALE/BZERO、拍摄信息、原像素、NaN、主图填充及其后的扩展 HDU 保持原字节。源文件只读；新文件采用排他创建。
- 来源、索引、求解器入口可执行文件及结果均记录 SHA-256。当前没有冻结求解器动态库及所有辅助程序；重新求解不是对完整软件运行环境的封存。导出既有结果不再调用外部求解器，保持产品字节一致。
- 本地进程使用独立进程组，取消与超时会终止辅助子进程。日志有大小上限。CLI 失败目录保留 `failure.txt`、星表和诊断日志，但不发布 `solved.fits` 或成功配方；App 的事务临时目录会回收。无解时应先检查索引、尺度、恒星数与母片质量，不应把退出码 0 单独当作成功。

## CLI

```sh
photonstack plate-solve --input master.fits \
  --solver /opt/homebrew/bin/solve-field \
  --index /path/index-4108.fits --index-frame ICRS \
  --output-dir new-solution --timeout 120

# 可选尺度提示；可以重复 --index 指定多个索引文件。
photonstack plate-solve --input master.fits \
  --solver /opt/homebrew/bin/solve-field \
  --index /path/index-4108.fits --index-frame ICRS \
  --scale-low 2 --scale-high 5 --output-dir new-solution-with-scale

photonstack plate-solve-inspect --recipe new-solution/recipe.txt
photonstack plate-solve-export --recipe new-solution/recipe.txt --output-dir copied-solution
photonstack plate-solve-replay --recipe new-solution/recipe.txt --output-dir recomputed-solution
```

`inspect` 校验原片和产品；`export` 校验并复制已审查结果；`replay` 另检查当前求解器与索引摘要，再重新求解。输出目录均须不存在。结果包含带坐标母片、完整星点/残差 JSON、训练星表、原始 WCS/匹配表/投影索引星及配方。索引本体不复制进结果目录。

独立数值检查可运行 `tools/scripts/qa/plate_solve_validation.py <结果目录>`，需要 NumPy/Astropy；它以 WCSLIB 复算坐标、统计及验证源数据与产品摘要。合成契约测试与真实外部求解的区别、实拍残差和性能记录见[完整验证记录](advanced-imaging-validation.zh-CN.md)。
