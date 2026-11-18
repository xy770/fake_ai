#!/usr/bin/env python3
# 把 chat.html / admin.html 打包成 frontend.h（C 字符串字面量），避免手写转义出错
import pathlib, sys

def enc(p):
    s = pathlib.Path(p).read_text(encoding='utf-8')
    # 压缩：去行首空白与换行，体积更小；保留必要的空格
    out = []
    for line in s.split('\n'):
        t = line.strip()
        if not t:
            continue
        out.append(t)
    s = ''.join(out)
    r = []
    for ch in s:
        if ch == '"': r.append('\\"')
        elif ch == '\\': r.append('\\\\')
        elif ch == '\n': r.append('\\n')
        elif ch == '\r': continue
        elif ord(ch) < 0x20: r.append('\\x%02x' % ord(ch))
        else: r.append(ch)
    return ''.join(r)

chat, admin = enc('chat.html'), enc('admin.html')
pathlib.Path('frontend.h').write_text(
    '/* frontend.h — 由 gen_frontend.py 自动生成，请勿手改 */\n'
    '#ifndef FAKEAI_FRONTEND_H\n#define FAKEAI_FRONTEND_H\n\n'
    f'static const char PAGE_CHAT[] = "{chat}";\n\n'
    f'static const char PAGE_ADMIN[] = "{admin}";\n\n'
    '#endif\n', encoding='utf-8')
print('frontend.h 生成完毕:', len(chat), 'bytes chat +', len(admin), 'bytes admin')
