用 NVIDIA DLSS 5 Neural Renderer（DLSS NR）神经网络对**视频与图片做逐帧画质增强**的本地工具。
纯本地运算，不上传任何数据；浏览器操作界面，无需安装。

> 🤖 **关于本软件**：这是 **Vibe Coding（对话式 AI 辅助开发）** 的产物——界面、引擎与视频处理
> 流程均由 AI 在人类引导下编写、评审与反复调优完成，人类负责需求定义、效果测试与发布决策。
> 欢迎 fork 与改进。

**v1.4 更新亮点**
- **图片批量渲染**（独立卡片）：选文件夹（含子目录）→ 整批图片按当前参数渲染到
  `nr_<原名>` 输出（可选 PNG/JPG），支持断点续跑（跳过已存在）
- **视频批量渲染**（独立卡片）：递归收集文件夹内所有视频 → 逐个加入渲染队列
  （同一进度条/队列面板，可调序/删除），各自用当前参数快照
- **显卡选择**：输出设置可列出系统全部显卡并手动指定渲染用卡，解决混合显卡
  笔记本"默认选核显跑不了模型"的问题（未指定时仍自动 NVIDIA 优先）
- 无图时图片对比黑框自动隐藏；闲置清理不再误删新渲染帧


> **使用提示**：源码仓库不含模型等二进制大文件（GitHub 单文件 100MB 限制）。
> 完整发行版已打包在 **GitHub Releases**（含引擎、界面、模型、深度运行库与内置运行时），
> 下载解压即可用，无需安装/改名。详见下文「快速开始」。

---
<img width="1539" height="1227" alt="图片" src="https://github.com/user-attachments/assets/2c5ef11f-6580-4cf8-9f15-68d230eac514" />



## 快速开始

### 路线 A：普通用户（免编译，推荐）

1. 打开本仓库 **Releases** 页面，下载最新版 **完整发行包**（如 `DLSS5NR_v1.4.zip`）；
2. 解压到任意目录（路径含中文也没问题）；
3. 双击 **（点击启动）Start_DLSS5NR.bat**；
4. 浏览器自动打开操作界面（地址 http://127.0.0.1:8777）；
5. 拖入/选择视频或图片 → 调参数 → 点「开始处理」/「渲染此图」；
6. 用完直接关闭黑色命令行窗口：服务停止，同时自动清理临时缓存。

完整发行包已内置：处理引擎、NR 模型、深度运行库、网页界面、便携 node/ffmpeg，解压即用。

### 路线 B：源码构建（开发者）

1. 克隆本仓库源码；
2. 到 **Releases** 下载 **完整发行包**，解压后将包内的 `models/`（NR 模型 + 转发器）与
   `core/depth/`（深度运行库）复制到源码根目录——文件名与位置已按引擎探测路径排好，无需改名；
3. 按下方「构建」一节用 MSVC 编译出 `core/dlss5nr_engine.exe`；
4. 确保 PATH 里有 `ffmpeg`/`ffprobe` 与 `node`，双击 `start_ui.bat` 运行。

### Releases 资产说明

| 资产 | 内容 | 适用 |
|---|---|---|
| 完整发行包 `DLSS5NR_v1.4.zip` | 引擎 + 界面 + NR 模型(fp16/fp8) + 深度运行库 + 便携 node/ffmpeg + 启动脚本 + 使用说明 | 所有用户：解压 → 双击启动 → 浏览器操作 |

包内 `models/` 与 `core/depth/` 已按引擎探测路径排好：`models/` 放 NR 模型与配套转发器
（`nvngx_dlssnr_fp16.dll` / `nvngx_dlssnr_fp8.dll` + `nvngx.dll_dlssnr_fp16.dll` /
`nvngx.dll_dlssnr_fp8.dll`，文件名勿改），`core/depth/` 为可选的深度推理运行库。

---

## 目录结构

```
core/                     C++ 处理引擎（D3D12 + ffmpeg 管道）
  main.cpp                入口：CLI、参数解析、渲染主循环
  d3d12_ctx.*             渲染上下文、纹理上传/回读
  dlssnr.*                NGX DLSS NR feature 加载与调用
  ngx_params.*            NGX 参数块构造
  nvof_flow.*             硬件光流(NV-OF, D3D11) → 稀疏网格
  densify_pass.*          D3D12 计算着色器：稀疏网格 → 全分辨率运动场
  blend_pass.*            GPU 残差混合 + Bayer 抖动降位
  depth_anything.*        深度推理（可选）
  video_pipe.*            ffmpeg 解码/编码子进程封装
  build.sh                MSVC 构建脚本
web/                      浏览器界面 + 本地服务（node，无第三方依赖）
server_guard.c            启动守护（关窗即清临时缓存）
start_ui.bat              开发环境启动脚本
```

## 构建（Windows）

要求：Visual Studio 2022（x64 `cl.exe`）、Windows 10 SDK、Git-Bash 或类似 bash。

```bash
cd core
bash build.sh          # 产物 core/dlss5nr_engine.exe
```

引擎通过 ffmpeg 子进程编解码，运行期还需要 PATH 里有 `ffmpeg`/`ffprobe` 与 `node`
（完整包里已内置，源码构建需自备）。

## 命令行参数

```bash
core/dlss5nr_engine.exe --input in.mp4 --output out.mp4 \
  --encoder h264_nvenc --residual-mult 1.0 --frame-guidance 3 \
  --end-time 5 --perf
```

常用：`--input --output --encoder --codec-args --pix-fmt --start-time --end-time
--preset --intensity --style --local-tone --local-structure --skin-structure
--auto-mask --ui-correction --residual-mult --frame-guidance --depth-interval
--dump-frame --frame-reset --hw-decode`。完整列表见 `--help`。

---

## 许可与致谢

本项目源码以 **GPL-3.0** 发布（见 `LICENSE`）。DLSSNR 的接入方式与参数方案参考了以下
开源项目，特此致谢：

- **Magpie** 的 DLSSNR 实验支持：开源 fork **SAOG0721/Magpie** 的 **`experimental` 分支**
  （https://github.com/SAOG0721/Magpie ，GPL-3.0；上游为 Blinue/Magpie）；
- **OptiScaler** 社区的 DLSSNR 支持实现（本工具早期调试所用的转发器 `nvngx.dll_dlssnr*.dll`
  源于其社区构建）。

界面、引擎与视频处理流程为本项目独立编写。

- `core/nvof/` 头文件：Copyright (c) 2018-2023 NVIDIA Corporation，宽松许可（见文件头）。
- `core/depth/onnxruntime_c_api.h`：ONNX Runtime 项目头文件，MIT 许可。
- **Releases 中的模型与转发器为社区/原作者作品**，打包发布前请自行确认其许可与
  NVIDIA 软件许可条款允许；请勿用于商业用途。

> 再分发或商用前，请自行完成对第三方组件的许可与合规核查。
