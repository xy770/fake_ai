# ∞ 无限额度 · 假AI（心理课小组展示用）

单文件 C99 实现，**零第三方依赖**，只用到 Linux 自带 syscall + pthread。
三层回复引擎：L1 ELIZA-1966 规则反问 → L2 内置语料润色 → **L3 真人坐席亲手打字**。
自带 A/B 两级安全熔断（A 级直接推 24h 心理热线并强制转人工）。
对话**只存内存，进程一退即焚**，不落盘、不写日志。

## Arch Linux 部署（3 分钟）

```bash
sudo pacman -S --needed base-devel        # gcc + make
tar xf faketoken_src.tar.gz && cd faketoken_release
make                                       # 零 warning

# 前台试跑（只监听本机）
FAKEAI_TOKEN=class2026 ./fakeai 127.0.0.1 9000

# 正式上课：监听 0.0.0.0 让同学手机能扫进来
FAKEAI_TOKEN=换个你自己的词 ./fakeai 0.0.0.0 9000
```

浏览器开 `http://<你电脑局域网IP>:9000/admin?token=你的FAKEAI_TOKEN` = **坐席控制台**。
学生扫 `http://<你电脑局域网IP>:9000` = 聊天页。二维码用任意工具生成这个 URL 即可。

> ⚠️ 校园网常开 AP 隔离，手机可能扫不到你电脑。**上课前一定先自己手机试一次**。
> 兜底方案：手机开热点给电脑连，或 `sudo pacman -S cloudflared && cloudflared tunnel --url http://localhost:9000` 拿一个 https 公网地址再生成二维码（最稳）。

## 参数

| 项 | 位置 | 说明 |
|---|---|---|
| 坐席口令 | 环境变量 `FAKEAI_TOKEN` | 不设则默认 `class2026`，**上课前务必改** |
| 监听 | `argv[1]` / `argv[2]` | 默认 `127.0.0.1 9000` |
| 单条上限 | `api_send` 里 `> 800` | 超长返回 413 |
| 最大并发 | `MAX_CLIENTS 512` | 够一个班 |
| L2 语料 | `fakeai.c` 里 `k_corpus[]` | **改成你们组的口吻，这步最重要** |
| A级红线词 | `k_red_A[]` | 命中即推热线 + 转人工 |
| B级预警词 | `k_red_B[]` | 只点红灯进队列，不打断对话 |

## 上课流程（15 分钟）

1. 0-2′ 投二维码，不说是真是假，你在 /admin 亲手回
2. 5′ 点「交还AI」→ 温差出现（这是讨论引子）
3. 6′ 再切纯 L1 规则版问同一句 → 全场笑，讲**伊丽莎效应**
4. 7′ 揭晓：大屏投 /admin 实时消息流，"最戳你那句是我同学打的"
5. 10-14′ 三问 → 最后两两配对互发一条，把"无限回应"还给真人

## 伦理（老师看的就是这页）

- 开场加一句："对面可能不是人，**你随时可以问'你是人吗'，它会诚实回答**"
- 下课必须 debrief 告知真相
- 对话不落库不截图；`/admin` 未带 token 一律 403
- 红线命中 → 立刻线下走过去拍拍他，比任何自动回复都值钱

## 改前端

`chat.html` / `admin.html` 改完执行 `make frontend`，会重新内联进 `frontend.h` 再 `make`。
⚠️ 别在 `chat.html`/`admin.html` 是空文件时跑 gen_frontend.py，会把 frontend.h 覆盖空。
