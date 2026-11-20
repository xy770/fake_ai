# Fake AI · 用不完的 Token

心理课作业。一个"只认词、不认人"的假 AI 聊天站：没有模型，没有语料，只有规则、模板和一点小聪明。

## 回复优先级（高到低）

1. 招呼池：你好/在吗/hi…
2. REPEAT：与环形记忆中原话完全重复 → 点破"第 N 遍"。
3. REGEX 规则组：按 config 里 rules 顺序匹配，命中取模板轮转；每 4 轮追加一句 RING 追问（不抢主回答）。
4. CAPABILITY："你会/你能…吗" → 自曝技能栏。
5. RING 旧话重提：仅当未命中关键词；候选过滤问句/语气词/已回声项，按情绪权重×2+新近度打分挑一句。
6. QUESTION：问句走反问池。
7. PRONOUN FLIP：陈述句做 我↔你/我们↔你们/我的↔你的 同步互换。
8. 兜底池。

情绪线（附加在主回答后）：lexicon 逐轮打分；连负 3 轮追加 care；负转正追加 lift；命中 bye_rule 时输出今日总结（句数 + 分量最重一句）。

## 可扩展性

- 全部规则、模板池、情绪词表、门牌号在 `config.json`（embed 为默认值）。
- 服务 WorkingDirectory 下放 `rules.json` 即覆盖（按键合并：room/bye_rule/rules 整段覆盖，pools/lexicon 按子键覆盖，缺省回退默认）。
- 热reload：`sudo systemctl kill -s HUP fake_ai`，不重启进程、不断会话。
- 模板槽位：{kw} 命中词、{flip} 人称翻转句、{old} 记忆/重复句、{n} 计数、{room} 门牌。

## 运行

    go build -o fake_ai .
    ./fake_ai -addr 127.0.0.1:8787 -config rules.json

systemd：deploy/fake_ai.service；nginx 反代参考 deploy/nginx.fake_ai.conf。
改代码后：go build -o fake_ai . && sudo systemctl restart fake_ai
只改配置：sudo systemctl kill -s HUP fake_ai

## 自测

    curl -s -c /tmp/jar -b /tmp/jar -X POST http://127.0.0.1:8787/api/chat \
      -H 'Content-Type: application/json' -d '{"message":"我今天好累，不想写作业"}'

## 目录

    main.go     HTTP 服务、session、环形记忆存储
    config.go   配置加载/合并/热reload
    config.json 默认配置（规则/模板/词表/门牌）
    fakeai.go   回复优先级与情绪线逻辑
    web/        前端（embed 进二进制）
    deploy/     systemd unit 与 nginx 片段
