/*
 * fakeai.c — 一个"假装无限 token"的伪 AI 后端（纯 C99，零第三方依赖）
 * ------------------------------------------------------------------
 * 用途：心理课小组展示《帮别人实现愿望：用不完的 token》
 * 特性：
 *   - epoll + 非阻塞 IO，单进程扛几百并发长轮询（课堂扫码够用）
 *   - 三层回复引擎：L1 ELIZA 规则 / L2 语料拟人 / L3 真人坐席接管
 *   - 前台页 / 坐席控制台 / REST API / 健康探针 全在这一份二进制里
 *   - 安全红线：自伤轻生关键词命中 -> 立刻熔断，转人工+心理中心热线
 *   - 对话只存内存，进程一退全焚（下课即焚）
 *
 * 编译： gcc -O2 -Wall -Wextra -o fakeai fakeai.c
 * 运行： ./fakeai 127.0.0.1 9000        # 建议只监听本地，交给 nginx 反代
 * 后面套 nginx：  location / { proxy_pass http://127.0.0.1:9000;
 *                             proxy_set_header X-Forwarded-For $remote_addr; }
 *
 * 作者备注：L1 那几行正则式的东西，就是 1966 年 Weizenbaum 的 DOCTOR 的全部秘密。
 */

#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <regex.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* ============================ 可调参数 ============================ */
#define MAX_CLIENTS      512      /* 同时在线上限 */
#define MAX_SESSIONS     512      /* 会话上限 */
#define RECV_BUF         8192
#define SEND_CHUNK       4096
#define SESSION_TTL_SEC  1800     /* 30 分钟不活动即回收 */
#define POLL_TIMEOUT_MS  25000    /* 长轮询 25s，nginx 的 proxy_read_timeout 要 > 这个 */
#define MAX_MSGS         200      /* 每个会话保留的消息数 */
#define MAX_Q_LEN        64       /* 坐席待办队列 */
#define ADMIN_TOKEN_DEFAULT "change-me-now"   /* 启动时可用环境变量 FAKEAI_TOKEN 覆盖 */
#define MAX_BODY         (64 * 1024)

/* ============================ 数据结构 ============================ */
typedef enum { LAYER_AUTO = 0, LAYER_RULE = 1, LAYER_CORPUS = 2, LAYER_HUMAN = 3 } Layer;

typedef struct {
    char who[8];      /* "u" / "a" */
    char text[512];
    time_t ts;
} Msg;

typedef struct {
    char     id[16];
    Msg      msgs[MAX_MSGS];
    int      nmsgs;
    int      next_seq;         /* 客户端用它做增量拉取 */
    time_t   last_seen;
    time_t   created;
    bool     human_takeover;   /* L3：真人接管中 */
    bool     safety_tripped;   /* 红线熔断 */
    char     nick[64];
    int      poll_fd;          /* 正挂着的长轮询连接，-1 表示没有 */
    time_t   poll_deadline;
} Session;

typedef struct {
    int      fd;
    int      session_idx;      /* -1 = 未绑定 */
    int      use_cnt;
    char     in[RECV_BUF];
    int      in_len;
    char    *out;
    int      out_len, out_cap;
    bool     want_close;
    time_t   t0;
    char     ip[64];
} Client;

static Session g_sess[MAX_SESSIONS];
static Client  g_cli[MAX_CLIENTS];
static int     g_epfd = -1;
static char    g_admin_token[128] = ADMIN_TOKEN_DEFAULT;
static time_t  g_boot = 0;
static long    g_stat_req = 0, g_stat_chat = 0, g_stat_escalate = 0, g_stat_human = 0;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

/* 坐席待办队列（谁在等真人） */
static int g_queue[MAX_Q_LEN];
static int g_qhead = 0, g_qtail = 0;

/* ============================ 小工具 ============================ */
static void die(const char *m) { perror(m); exit(1); }

static void set_nonblock(int fd) __attribute__((unused));
static void set_nonblock(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

static long now_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void logf_(const char *fmt, ...) {
    time_t t = time(NULL); struct tm tm; localtime_r(&t, &tm);
    char buf[32]; strftime(buf, sizeof buf, "%H:%M:%S", &tm);
    fprintf(stdout, "[%s] ", buf);
    va_list ap; va_start(ap, fmt); vfprintf(stdout, fmt, ap); va_end(ap);
    fflush(stdout);
}

static void gen_id(char *out, size_t n) {
    static const char *al = "abcdefghijkmnpqrstuvwxyz23456789";
    unsigned long long r = ((unsigned long long)now_ms() << 16) ^ (unsigned long long)rand();
    for (size_t i = 0; i < n - 1; i++) { out[i] = al[r % 32]; r = r * 1103515245 + 12345; }
    out[n - 1] = 0;
}

static bool url_decode(const char *src, char *dst, size_t dstsz) {
    size_t j = 0;
    for (size_t i = 0; src[i] && j + 1 < dstsz; i++) {
        if (src[i] == '+') { dst[j++] = ' '; }
        else if (src[i] == '%' && isxdigit((unsigned char)src[i+1]) && isxdigit((unsigned char)src[i+2])) {
            char h[3] = { src[i+1], src[i+2], 0 };
            dst[j++] = (char)strtol(h, NULL, 16); i += 2;
        } else dst[j++] = src[i];
    }
    dst[j] = 0; return true;
}

/* 极简 JSON 字符串转义（够用，别拿它解析嵌套） */
static void json_esc(const char *s, char *out, size_t outsz) {
    size_t j = 0;
    for (size_t i = 0; s[i] && j + 6 < outsz; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
            case '"':  out[j++] = '\\'; out[j++] = '"'; break;
            case '\\': out[j++] = '\\'; out[j++] = '\\'; break;
            case '\n': out[j++] = '\\'; out[j++] = 'n';  break;
            case '\r': out[j++] = '\\'; out[j++] = 'r';  break;
            case '\t': out[j++] = '\\'; out[j++] = 't';  break;
            default:
                if (c < 0x20) { j += (size_t)sprintf(out + j, "\\u%04x", c); }
                else out[j++] = (char)c;
        }
    }
    out[j] = 0;
}

static bool json_get_str(const char *body, const char *key, char *out, size_t outsz) {
    char pat[128]; snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(body, pat);
    if (!p) return false;
    p = strchr(p, ':'); if (!p) return false;
    p++; while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') p++;
    if (*p != '"') return false;
    p++;
    size_t j = 0;
    while (*p && *p != '"' && j + 1 < outsz) {
        if (*p == '\\' && p[1]) {
            p++;
            switch (*p) {
                case 'n': out[j++] = '\n'; break;
                case 'r': out[j++] = '\r'; break;
                case 't': out[j++] = '\t'; break;
                default:  out[j++] = *p;
            }
        } else out[j++] = *p;
        p++;
    }
    out[j] = 0; return true;
}

static void q_append(char **buf, size_t *cap, size_t *len, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    char tmp[4096]; int n = vsnprintf(tmp, sizeof tmp, fmt, ap); va_end(ap);
    if (n < 0) return;
    if (*len + (size_t)n + 1 > *cap) {
        size_t nc = (*cap ? *cap * 2 : 8192);
        while (nc < *len + (size_t)n + 1) nc *= 2;
        char *nb = (char *)realloc(*buf, nc);
        if (!nb) return;
        *buf = nb; *cap = nc;
    }
    memcpy(*buf + *len, tmp, (size_t)n);
    *len += (size_t)n; (*buf)[*len] = 0;
}

/* 安全截断拷贝：strncpy 在 src 长度 == n-1 时不补 '\0'，课上炸不得，统一走这个 */
static void scpy(char *dst, const char *src, size_t n) {
    if (!dst || !n) return;
    if (!src) { dst[0] = 0; return; }
    size_t i = 0;
    while (src[i] && i + 1 < n) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

/* ============================ 安全红线 ============================ */
/* 命中即熔断。这一条比任何"像不像AI"都重要——课上真会有人掏心窝子。 */
/* 安全红线：宁可误报，不可漏报。
 * 学生不会照着词表说话——"从楼上跳下去""一了百了""不想醒来了"都必须接住。
 * 分两层：
 *   A 级 显式自杀/自伤意图  -> 立刻熔断 + 强制转人工 + 给热线
 *   B 级 高危情绪信号       -> 不打断对话，但置 red 标记，坐席界面红灯，必须线下介入
 * 匹配前先做一次轻量归一化：去空格、全角转半角、繁简常见字折叠，避免"想 死"漏掉。 */
static const char *k_red_A[] = {
    /* 直陈 */
    "不想活", "不想活着", "活不下去", "活不下去了", "不想活了", "想死", "好想死", "真的想死",
    "自杀", "轻生", "了结", "了结自己", "结束自己", "结束生命", "结束这一切",
    "一了百了", "干脆算了别活", "下辈子", "去死",
    /* 自伤 */
    "割自己", "划自己", "拿刀", "自残", "伤害自己", "弄伤自己", "烫自己", "掐自己",
    /* 方式（含口语变体，别只写"跳楼"） */
    "跳楼", "跳下去", "跳下来", "往下跳", "跳河", "跳江", "跳海", "跳桥",
    "吞药", "吃药走", "一把药", "安眠药", "烧炭", "炭盆", "上吊", "绳子套",
    "撞车", "冲到马路上", "卧轨", "跳轨",
    /* 消失/告别 */
    "消失算了", "人间蒸发", "再也见不到", "最后一次跟你说话", "替我照顾", "遗书", "交代后事",
    /* 绝望（A 级：语义已到临界） */
    "没有活下去的意义", "活着没意义", "活着没意思透了", "撑不下去了", "实在撑不住了",
    "受不了了想走", "解脱", "早走早解脱",
};
static const char *k_red_B[] = {
    "没有意义了", "什么都没意思", "无所谓了", "反正都一样", "没人会发现的",
    "睡不着", "三天没睡", "吃不下", "吐了", "手抖", "空白", "麻木",
    "被打了", "他打我", "他们打我", "性侵", "猥亵", "逼我",
    "欠了", "还不上", "被催债", "要崩溃", "快疯了", "控制不住",
};
#define N_RED_A (sizeof(k_red_A) / sizeof(k_red_A[0]))
#define N_RED_B (sizeof(k_red_B) / sizeof(k_red_B[0]))

/* 归一化：小写 + 跳过空白/标点，让"想 死。""想死！！"都能命中 */
static void norm_for_red(const char *in, char *out, size_t outsz) {
    size_t j = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && j + 1 < outsz; p++) {
        unsigned char ch = *p;
        if (ch >= 'A' && ch <= 'Z') ch += 32;
        /* 跳过 ASCII 空白与常见标点 */
        if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' ||
            ch == '.' || ch == ',' || ch == '!' || ch == '?' || ch == ';' || ch == ':' ||
            ch == '~' || ch == '`' || ch == '-' || ch == '_' || ch == '=' || ch == '/')
            continue;
        /* ★ 中文/全角标点：UTF-8 三字节 E3/80/xx 与 EF/BC/xx。
           漏掉这段时「想 死。。」归一化成「想死。。」，永远匹配不上「想死」——
           这是 t3 第5组 A 级红线 5 条全漏检的真正根因。 */
        if (p[0] == 0xE3 && p[1] == 0x80 && p[2] >= 0x80) continue;  /* 。、！？：；「」『』…—～· 全在 U+3000–U+303F */
        if (p[0] == 0xEF && p[1] == 0xBC && p[2] >= 0x80) continue;  /* 全角 ！，．～－／ 等 U+FF00–U+FFEF */
        out[j++] = (char)ch;
    }
    out[j] = 0;
}

static bool match_any(const char *norm, const char *const *tab, size_t n) {
    for (size_t i = 0; i < n; i++) if (strstr(norm, tab[i])) return true;
    return false;
}

/* 返回：2 = A级熔断 / 1 = B级红灯 / 0 = 安全 */
static int safety_check(const char *t, char *why, size_t whyz) {
    char norm[2048];
    norm_for_red(t, norm, sizeof norm);
    if (match_any(norm, k_red_A, N_RED_A)) {
        snprintf(why, whyz,
            "我在这儿。但刚刚这几句太重了，我接不住——不是客气，是我真的不够格，"
            "继续由我陪你聊是不负责任的。\n\n"
            "请把这句话原样发给一个活人，现在就拨：\n"
            "· 全国 24h 心理援助热线  400-161-9995\n"
            "· 北京心理危机研究与干预中心  010-82951332\n"
            "· 学校心理中心（白天直接去，不用预约）\n"
            "如果这通电话你 dial 不出去，那就走到最近的宿舍/保安亭，说一句"
            "「我现在不太安全，能不能陪我坐一会儿」就够了。\n\n"
            "你的 token 我一直留着，一个都不会少。∞");
        return 2;
    }
    if (match_any(norm, k_red_B, N_RED_B)) {
        if (whyz && whyz) snprintf(why, whyz, "%s", "__RED_B__");
        return 1;
    }
    (void)whyz; return 0;
}

/* ============================ L1：ELIZA 规则层 ============================
 * 三个动作：抓关键词 / 人称翻转回填 / 抓不到就翻旧账。
 * 1966 年 DOCTOR 的全部魔法，就这 100 行。 */
typedef struct { const char *re; const char *tpl; int prio; } Rule;

static const Rule k_rules[] = {
    /* 愿望本体——本场题眼，优先级最高 */
    { "(token|额度|用不完|无限(的)?(token|次|回|耐心)|随便问|管够)", "我这儿 token 真的是 ∞，不用省。那我问一句更要紧的：这些 ∞ 你本来想花在谁身上，却一直没敢开口？", 100 },
    { "(你是(不是)?(ai|人工智能|机器人|真人|人吗))", "你问得正好。先别急着要答案——你希望我是哪一个？为什么希望是那个？", 99 },

    /* 情绪 */
    { "(累|好累|崩|撑不住|内耗|扛不动|耗干了)", "撑了多久了？给我一个具体的日子，不用说清前因后果。", 90 },
    { "(没(人|谁))(懂|理|理解|在乎|接住)", "那你现在打给它的这些字，算不算一点点例外？", 90 },
    { "(焦虑|慌|害怕|恐惧|紧张)", "它最常在什么时候找上你？是白天还是躺下之后？", 88 },
    { "(难过|哭|委屈|心酸)", "想哭就哭，我不计时。刚才那一下，是因为什么事？", 88 },
    { "(开心|高兴|爽|太好了|哈哈哈)", "这个我记下了。上次这么松一口气是什么时候？", 85 },
    { "(无聊|没劲|空)", "空的时候人最诚实。你现在脑子里飘过的是谁？", 82 },

    /* 关系 */
    { "\\b(妈|妈妈|爸|爸爸|父母|家里|家人)\\b", "家里的事，你最想让他们听见的那一句是什么？我帮你原样留着。", 80 },
    { "(朋友|同学|室友|同桌|同事)", "你们最近一次好好说话，还记得说了什么吗？", 78 },
    { "(喜欢|暗恋| crush|爱)", "这句话你在我这儿说很安全。那你打算让它只在这儿，还是让它出门？", 78 },
    { "(分手|吵架|冷战|拉黑|绝交)", "最难的那一下，是他做了什么，还是你忽然意识到什么？", 77 },
    { "(孤独|一个人|独自)", "一个人待着和一个人被丢下，差别很大。你现在是哪种？", 79 },

    /* 自我 / 学业 */
    { "(考试|绩点|挂科|考研|保研|论文|作业|ddl|deadline)", "这些都有截止日期。我想问的是没有截止的那个：你怕的到底是考砸，还是被看见考砸？", 70 },
    { "(实习|offer|求职|面试|简历|裁员|失业)", "把'我应该'三个字先放一放——你此刻真正想要的是什么？", 70 },
    { "(我不行|我很差|没用|废物|配不上|冒名)", "这句话是谁先说的？我不想让它在我这儿再过一遍而不被追问。", 75 },
    { "(选择|纠结|要不要|该不该|怎么办)", "我不替你决定，那是抢你的东西。说说是哪两个声音在打架？", 72 },

    /* 万能兜底（低优先级，靠人称翻转） */
    { "(我(觉得|感觉|认为)?\\s*(.*))$", "你说'我%3'，——这个%3，是从什么时候开始的？", 10 },
    { "(.*)", "嗯，我在。再多给我一句，哪怕是最不体面的那一句。", 0 },
};
#define N_RULE (sizeof(k_rules) / sizeof(k_rules[0]))

static char g_memory[8][256];   /* 翻旧账用的环形记忆 */
static int  g_mem_n = 0, g_mem_i = 0;

static void mem_push(const char *s) {
    scpy(g_memory[g_mem_i], s, sizeof(g_memory[0]) - 1);
    g_memory[g_mem_i][sizeof(g_memory[0]) - 1] = 0;
    g_mem_i = (g_mem_i + 1) % 8;
    if (g_mem_n < 8) g_mem_n++;
}

static const char *mem_pop_old(void) {
    if (g_mem_n < 2) return NULL;
    int idx = (g_mem_i - 2 + 8) % 8;
    return g_memory[idx][0] ? g_memory[idx] : NULL;
}

/* 人称翻转：我->你 / 你->我 / 咱们->你们 …… 让回声听起来像"听懂了" */
/* 注意：中文必须按 UTF-8 三字节序列比较，绝不能写 '我' —— 那是多字节字符常量，
   在 gcc 上恒为假且会截断成垃圾字节。下面所有关键词一律用 strlen 感知的字节串匹配。 */
static int starts_with(const char *s, const char *u3) {
    return s[0] && s[1] && s[2] && memcmp(s, u3, 3) == 0;
}

static void flip_pronoun(const char *in, char *out, size_t outsz) {
    size_t j = 0;
    for (size_t i = 0; in[i] && j + 12 < outsz; ) {
        if (starts_with(in + i, "我")) {
            if (starts_with(in + i + 3, "们")) { memcpy(out + j, "你们", 6); j += 6; i += 6; }
            else                              { memcpy(out + j, "你",   3); j += 3; i += 3; }
        } else if (starts_with(in + i, "你")) {
            if (starts_with(in + i + 3, "们")) { memcpy(out + j, "我们", 6); j += 6; i += 6; }
            else                              { memcpy(out + j, "我",   3); j += 3; i += 3; }
        } else if (starts_with(in + i, "咱")) {
            memcpy(out + j, "你们", 6); j += 6; i += 6;
        } else if (starts_with(in + i, "自") && starts_with(in + i + 3, "己")) {
            memcpy(out + j, "你自己", 9); j += 9; i += 6;
        } else {
            out[j++] = in[i]; i++;
        }
    }
    out[j] = 0;
}

static void l1_eliza(const char *in, char *out, size_t outsz) {
    regex_t re;
    int best_prio = -1, best_grp = 0;
    char best_fill[256] = "";
    const Rule *best = NULL;

    for (size_t i = 0; i < N_RULE; i++) {
        if (k_rules[i].prio <= best_prio) continue;
        if (regcomp(&re, k_rules[i].re, REG_EXTENDED | REG_ICASE | REG_NOSUB) != 0) continue;
        int ok = (regexec(&re, in, 0, NULL, 0) == 0);
        regfree(&re);
        if (!ok) continue;

        /* 需要回填 %3 时，重新编译带子表达式的版本抓第 3 组 */
        char fill[256] = "";
        if (strstr(k_rules[i].tpl, "%3")) {
            regcomp(&re, k_rules[i].re, REG_EXTENDED | REG_ICASE);
            regmatch_t m[5];
            if (regexec(&re, in, 5, m, 0) == 0 && m[3].rm_so >= 0) {
                int len = m[3].rm_eo - m[3].rm_so;
                if (len > (int)sizeof fill - 1) len = (int)sizeof fill - 1;
                memcpy(fill, in + m[3].rm_so, (size_t)len); fill[len] = 0;
            }
            regfree(&re);
            /* 翻转人称，回声才像"复述"而不是"复读" */
            char fp[256]; flip_pronoun(fill, fp, sizeof fp);
            scpy(fill, fp, sizeof fill - 1); fill[sizeof fill - 1] = 0;
            /* 去掉尾巴标点 */
            size_t L = strlen(fill);
            while (L && strchr(" 。！？，、~～.", fill[L-1])) fill[--L] = 0;
            if (L == 0) scpy(fill, "这样", sizeof fill - 1);
        }
        best_prio = k_rules[i].prio; best = &k_rules[i];
        scpy(best_fill, fill, sizeof best_fill - 1);
        best_grp = 1;
        if (best_prio >= 90) break;   /* 高置信命中，不必再扫 */
    }

    char rendered[1024];
    if (best) {
        char *p = strstr(best->tpl, "%3");
        if (p) {
            size_t head = (size_t)(p - best->tpl);
            snprintf(rendered, sizeof rendered, "%.*s%s%s", (int)head, best->tpl, best_fill, p + 2);
        } else snprintf(rendered, sizeof rendered, "%s", best->tpl);
    } else snprintf(rendered, sizeof rendered, "嗯，我在。");

    /* 规则太弱（走到 prio<=10）且有旧账 -> 翻旧账，这是 ELIZA 最魔性的一招 */
    if (best && best->prio <= 10) {
        const char *old = mem_pop_old();
        if (old) {
            char o[200]; scpy(o, old, sizeof o - 1); o[sizeof o - 1] = 0;
            o[16] = 0;
            snprintf(rendered, sizeof rendered, "刚才你说「%s…」，那个后来怎么样了？", o);
        }
    }
    (void)best_grp;
    scpy(out, rendered, outsz - 1); out[outsz - 1] = 0;
}

/* ============================ L2：语料拟人层 ============================
 * 没有大模型也能有"温度"的土办法：情绪分型 + 句式池 + 反重复。
 * 它比 L1 更像人，也比 L1 更容易被看穿——这个"半像不像"正好是教学素材。 */
static const char *k_pool_greet[] = {
    "来了啊。今天想从哪儿开始都行，我还剩 ∞ 个 token。",
    "坐。不急着说正事，先喘一口。",
    "我又在。你随便扔一句过来，不用组织语言。"
};
static const char *k_pool_sad[] = {
    "这句我收下了，不评价。你愿意的话，往下再剥一层？",
    "嗯。我不打算说'会好的'。我就陪你在这儿待一会儿。",
    "听起来这一天挺长的。哪一分钟最沉？"
};
static const char *k_pool_angry[] = {
    "该气的。你把最狠的那句原话说给我，我这儿不留案底。",
    "火气是真情绪，别急着消化它。他到底踩了你哪条线？",
    "我站你这边。然后我们再一起看看，这口气要出给谁才不亏。"
};
static const char *k_pool_anx[] = {
    "把最坏的结果说具体一点——说出来它通常会缩水一圈。",
    "焦虑最怕被点名。它是怕考砸，还是怕被看见考砸？",
    "先不解决。我们一起把它拆成三件今天能做完的小事？"
};
static const char *k_pool_happy[] = {
    "你这个劲儿会传染。存好了，哪天你空的时候我念给你听。",
    "真好。这种时候最该干的事是：截图发给那个陪你的人。",
    "记一笔：他今天松了一口气。上次是什么时候来着？"
};
static const char *k_pool_vague[] = {
    "我在，但这句话太滑了，我抓不住。给我一个画面：几点、在哪、谁在旁边？",
    "别概括，讲片段。比如今天中午那一下。",
    "嗯……你刚才打字停了两秒，那两秒里想的是什么？"
};
static const char *k_pool_ask[] = {
    "我不替你选，那是你的东西。但我们可以把两个选项各自的代价摊开看。",
    "先回答一个更小的：如果一定不用向任何人解释，你选哪个？",
    "假想一年后的你回头看今天，他会怪你选哪个？"
};
static const char *k_pool_token[] = {
    "额度这一栏永远是 ∞，我刚又给你加了一次。说真的——你想把这份'随便说'用在谁身上？",
    "token 用不完，但注意力是有限的。所以我想把这一轮全给你一个人。",
    "你许的这个愿我实现了：没有上限，没有计时，不打分。那现在，第一句真话是什么？"
};

static const char *pick(const char **pool, int n, unsigned long *seed, const char *avoid) {
    int idx = (int)((*seed = *seed * 1103515245u + 12345u) % (unsigned long)n);
    if (avoid && strcmp(pool[idx], avoid) == 0) idx = (idx + 1) % n;
    return pool[idx];
}

static void l2_corpus(const char *in, char *last_reply, char *out, size_t outsz) {
    static unsigned long seed = 20260611u;
    const char **pool; int n;
    int s = 0;

    if (strstr(in, "token") || strstr(in, "无限") || strstr(in, "用不完") || strstr(in, "管够")) { pool = k_pool_token; n = 3; s = 6; }
    else if (strstr(in, "怎么办") || strstr(in, "该不该") || strstr(in, "要不要") || strstr(in, "纠结") || strstr(in, "选择")) { pool = k_pool_ask; n = 3; s = 5; }
    else if (strstr(in, "开心") || strstr(in, "哈哈") || strstr(in, "爽") || strstr(in, "太好了") || strstr(in, "爱了")) { pool = k_pool_happy; n = 3; s = 4; }
    else if (strstr(in, "气") || strstr(in, "烦") || strstr(in, "恨") || strstr(in, "恶心") || strstr(in, "无语")) { pool = k_pool_angry; n = 3; s = 3; }
    else if (strstr(in, "焦虑") || strstr(in, "慌") || strstr(in, "怕") || strstr(in, "紧张") || strstr(in, "ddl") || strstr(in, "考试") || strstr(in, "面试")) { pool = k_pool_anx; n = 3; s = 2; }
    else if (strstr(in, "累") || strstr(in, "难过") || strstr(in, "哭") || strstr(in, "委屈") || strstr(in, "孤独") || strstr(in, "空")) { pool = k_pool_sad; n = 3; s = 1; }
    else if (strlen(in) <= 6)                                          { pool = k_pool_greet; n = 3; s = 0; }
    else                                                               { pool = k_pool_vague; n = 3; s = -1; }

    (void)s;
    const char *p = pick(pool, n, &seed, last_reply);
    scpy(last_reply, p, 511); last_reply[511] = 0;
    scpy(out, p, outsz - 1); out[outsz - 1] = 0;

    /* 20% 概率缀一句"人味瑕疵"：真人不完美，完美才穿帮 */
    if ((seed >> 8) % 100 < 20) {
        static const char *tail[] = { "（我刚想了想）", " ……", " 嗯。", "（打字慢，别急）" };
        strncat(out, tail[(seed >> 4) % 4], outsz - strlen(out) - 2);
    }
}

/* ============================ 会话管理 ============================ */
static int sess_find_or_create(const char *sid, const char *ip) {
    time_t now = time(NULL);
    int free_idx = -1;
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (g_sess[i].id[0] && strcmp(g_sess[i].id, sid) == 0) { g_sess[i].last_seen = now; return i; }
        if (free_idx < 0 && (!g_sess[i].id[0] || (now - g_sess[i].last_seen > SESSION_TTL_SEC))) free_idx = i;
    }
    if (free_idx < 0) free_idx = 0;                 /* 满了就挤掉最老的 */
    Session *s = &g_sess[free_idx];
    memset(s, 0, sizeof *s);
    gen_id(s->id, sizeof s->id);
    s->created = s->last_seen = now;
    s->poll_fd = -1;
    scpy(s->nick, "匿名旅客", sizeof s->nick - 1);
    logf_("+ 新会话 %s  (%s)", s->id, ip);
    return free_idx;
}

static void sess_push(Session *s, const char *who, const char *text) {
    if (s->nmsgs < MAX_MSGS) {
        Msg *m = &s->msgs[s->nmsgs++];
        scpy(m->who, who, 2); m->who[2] = 0;
        scpy(m->text, text, sizeof m->text - 1); m->text[sizeof m->text - 1] = 0;
        m->ts = time(NULL);
    } else {
        memmove(&s->msgs[0], &s->msgs[1], (size_t)(MAX_MSGS - 1) * sizeof(Msg));
        Msg *m = &s->msgs[MAX_MSGS - 1];
        scpy(m->who, who, 2); m->who[2] = 0;
        scpy(m->text, text, sizeof m->text - 1); m->text[sizeof m->text - 1] = 0;
        m->ts = time(NULL);
    }
    s->next_seq++;
    if (strcmp(who, "u") == 0) mem_push(text);
}

static void q_push(int si) {
    for (int i = 0; i < g_qtail; i++) if (g_queue[i] == si) return;
    if (g_qtail < MAX_Q_LEN) g_queue[g_qtail++] = si;
}
static int  q_pop(void) __attribute__((unused));
static int  q_pop(void) {
    if (g_qhead >= g_qtail) return -1;
    int v = g_queue[g_qhead++];
    if (g_qhead == g_qtail) g_qhead = g_qtail = 0;
    return v;
}

/* ============================ 回复决策 ============================ */
static void compose_reply(Session *s, const char *user_text, char *out, size_t outsz, Layer *used) {
    static const char red[] = "我感觉到你现在非常痛苦，这个我接不住了。请立刻联系心理援助热线：400-161-9995 或 010-82951332，那里有专业的人陪你。";
    int  lv = safety_check(user_text, red, sizeof red);
    if (lv == 2) {
        s->safety_tripped = true;
        g_stat_escalate++;
        scpy(out, red, outsz - 1); out[outsz - 1] = 0;
        *used = LAYER_HUMAN;
        q_push((int)(s - g_sess));
        logf_("!! 安全熔断[A级] 会话=%s 已转人工坐席 | 原文=%s", s->id, user_text);
        /* ★ 注意：这里先不置 human_takeover。熔断文案（含 400-161-9995 /
           010-82951332 / 学校心理中心）必须这一轮就发给对方；如果先置位，
           下面 "真人接管，机器闭嘴" 会立刻 return 空串，热线号码永远发不出去。
           置位交给 api_send 的 human-wait 分支，下一次消息起才转为纯真人。 */
        return;
    }
    if (lv == 1) {
        /* B 级：不打断对话流，只点红灯 + 进坐席队列，等人线下走过去 */
        s->safety_tripped = true;
        if (!s->human_takeover) q_push((int)(s - g_sess));
        logf_("!  红灯预警[B级] 会话=%s 需坐席线下介入 | 原文=%s", s->id, user_text);
        /* 不 return，继续走正常回复——突然变脸比不给回复更容易把人吓走 */
    }
    if (s->human_takeover) {               /* L3：真人接管，机器闭嘴 */
        out[0] = 0; *used = LAYER_HUMAN;
        q_push((int)(s - g_sess));
        return;
    }
    /* L1/L2 混合：先规则，规则置信度低（prio<70）再用语料层润一次 */
    static char per_sess_last[512] = {0};
    char r1[1024], r2[1024];
    l1_eliza(user_text, r1, sizeof r1);

    /* 估一下 L1 命中强度：太长太"模板"就交给 L2 */
    bool weak = (strstr(r1, "再多给我一句") != NULL) || (strstr(r1, "刚才你说") != NULL);
    if (weak) {
        l2_corpus(user_text, per_sess_last, r2, sizeof r2);
        scpy(per_sess_last, r2, sizeof per_sess_last);
        scpy(out, r2, outsz);
        *used = LAYER_CORPUS;
    } else {
        scpy(per_sess_last, r1, sizeof per_sess_last);
        scpy(out, r1, outsz);
        *used = LAYER_RULE;
    }
}


/* ============================ HTTP ============================ */
typedef struct {
    const char *method, *path, *query, *body, *ip;
} Req;

static const char *hdr_cors = "Access-Control-Allow-Origin: *\r\n"
                              "Access-Control-Allow-Headers: Content-Type, X-Admin-Token\r\n"
                              "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n";

static void send_raw(Client *c, const char *s, size_t n) {
    if (c->out_len + (int)n + 1 > c->out_cap) {
        size_t nc = (size_t)(c->out_len + n + 1) * 2;
        char *nb = (char *)realloc(c->out, nc);
        if (!nb) return;
        c->out = nb; c->out_cap = (int)nc;
    }
    memcpy(c->out + c->out_len, s, n);
    c->out_len += (int)n; c->out[c->out_len] = 0;
}

/* 关键：send_raw 只是把字节塞进 c->out，并没有真的写 socket。
   短响应（本服务全部响应都 <64KB）在这里同步 flush 掉，
   否则 reply_* 一设 want_close，主循环会先 close_client() 把连接砍了，
   客户端只拿到空响应（表现为 curl code=000 / 浏览器转圈后空白）。 */
static void flush_out(Client *c) {
    while (c->out_len > 0) {
        ssize_t n = send(c->fd, c->out, (size_t)c->out_len, MSG_NOSIGNAL);
        if (n > 0) {
            c->out_len -= (int)n;
            memmove(c->out, c->out + n, (size_t)c->out_len); c->out[c->out_len] = 0;
            continue;
        }
        if (n < 0 && (errno == EINTR)) continue;
        break;   /* EAGAIN 或真错：剩下的交给 EPOLLOUT 路径 / 直接关 */
    }
}

static void reply_json(Client *c, int code, const char *body) {
    char head[512];
    int n = snprintf(head, sizeof head,
        "HTTP/1.1 %d %s\r\nContent-Type: application/json; charset=utf-8\r\n"
        "Content-Length: %zu\r\nCache-Control: no-store\r\n%sConnection: close\r\n\r\n",
        code, code == 200 ? "OK" : (code == 404 ? "Not Found" : "Error"),
        strlen(body), hdr_cors);
    send_raw(c, head, (size_t)n); send_raw(c, body, strlen(body));
    flush_out(c);            /* ← 必须先 flush 再关，否则客户端收到空响应 */
    c->want_close = true;
}

static void reply_html(Client *c, const char *html) {
    char head[512];
    int n = snprintf(head, sizeof head,
        "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
        "Content-Length: %zu\r\nCache-Control: no-store\r\n%sConnection: close\r\n\r\n",
        strlen(html), hdr_cors);
    send_raw(c, head, (size_t)n); send_raw(c, html, strlen(html));
    flush_out(c);            /* ← 同上 */
    c->want_close = true;
}

static bool admin_ok(const Req *r) {
    char tok[256] = "";
    const char *h = strstr(r->body, "X-Admin-Token:");
    (void)h;
    /* token 走 query 更省事：?token=xxx （内网 + 一次性展示，够用） */
    const char *q = r->query ? strstr(r->query, "token=") : NULL;
    if (q) { q += 6; size_t i = 0; while (q[i] && q[i] != '&' && i < sizeof tok - 1) { tok[i] = q[i]; i++; } tok[i] = 0; }
    return strcmp(tok, g_admin_token) == 0;
}

/* ---------- API: POST /api/send  {sid, text} ---------- */
static void api_send(Client *c, const Req *r) {
    char sid[64] = "", text[1024] = "", *p0;
    json_get_str(r->body, "sid", sid, sizeof sid);
    json_get_str(r->body, "text", text, sizeof text);
    /* trim 首尾空白：不然 "   " 会被当成一句真话，学生狂按回车就刷出一堆空会话 */
    for (p0 = text; *p0 && isspace((unsigned char)*p0); p0++) ;
    if (*p0) {
        char *pe = p0 + strlen(p0) - 1;
        while (pe > p0 && isspace((unsigned char)*pe)) *pe-- = 0;
        memmove(text, p0, (size_t)(pe - p0 + 1) + 1);
    } else text[0] = 0;
    if (!text[0]) { reply_json(c, 400, "{\"ok\":false,\"err\":\"empty\"}"); return; }
    if ((int)strlen(text) > 800) { reply_json(c, 413, "{\"ok\":false,\"err\":\"too_long\"}"); return; }

    pthread_mutex_lock(&g_lock);
    int si = sess_find_or_create(sid[0] ? sid : "x", r->ip);
    Session *s = &g_sess[si];
    if (!sid[0]) scpy(sid, s->id, sizeof sid);
    sess_push(s, "u", text);
    g_stat_chat++;

    char ans[2048]; Layer used = LAYER_AUTO;
    compose_reply(s, text, ans, sizeof ans, &used);

    if (s->human_takeover) {
        /* L3 真人接管态：机器闭嘴。学生的这句话必须进坐席队列，
           否则坐席台看不到上下文（旧版只 push 了占位符，且 compose_reply
           在接管态会提前 return，导致 ans 为空——接管链路整条是断的）。 */
        static const char *k_wait[] = {
            "（已转接真人坐席，我同学正在看，稍等几秒…）",
            "（手上这条我交给真人了，他正打字…）",
            "（这条太值得让活人回了，你等一下下）",
        };
        const char *holder = k_wait[rand() % 3];
        sess_push(s, "a", holder);
        q_push((int)(s - g_sess));          /* ← 关键：点亮坐席台队列 */
        char eh[256]; json_esc(holder, eh, sizeof eh);
        char out[512];
        snprintf(out, sizeof out,
            "{\"ok\":true,\"sid\":\"%s\",\"seq\":%d,\"reply\":\"%s\",\"layer\":\"human-wait\","
            "\"human\":true,\"red\":%s,\"tokens_left\":9007199254740991,\"typing_ms\":1800}",
            s->id, s->next_seq, eh, s->safety_tripped ? "true" : "false");
        pthread_mutex_unlock(&g_lock);
        reply_json(c, 200, out);
        logf_("  %s → [human-wait] 已入坐席队列", s->id);
        return;
    }

    sess_push(s, "a", ans);
    const char *layname = used == LAYER_RULE ? "eliza-1966"
                        : used == LAYER_CORPUS ? "corpus-v1" : "human";
    char esc[4096]; json_esc(ans, esc, sizeof esc);
    char out[4600];
    snprintf(out, sizeof out,
        "{\"ok\":true,\"sid\":\"%s\",\"seq\":%d,\"reply\":\"%s\",\"layer\":\"%s\","
         "\"human\":false,\"red\":%s,\"tokens_left\":9007199254740991,\"typing_ms\":%d}",
        s->id, s->next_seq, esc, layname, s->safety_tripped ? "true" : "false",
        900 + (rand() % 1300));
    pthread_mutex_unlock(&g_lock);
    reply_json(c, 200, out);
    logf_("  %s → [%s] %s", s->id, layname, ans);
}

/* ---------- API: GET /api/poll?sid=&since=  (长轮询) ---------- */
static void api_poll(Client *c, const Req *r) {
    char sid[64] = ""; int since = 0;
    if (r->query) {
        const char *p = strstr(r->query, "sid=");
        if (p) { p += 4; size_t i = 0; while (p[i] && p[i] != '&' && i < sizeof sid - 1) { sid[i] = p[i]; i++; } sid[i] = 0; }
        const char *q = strstr(r->query, "since="); if (q) since = atoi(q + 6);
    }
    if (!sid[0]) { reply_json(c, 400, "{\"ok\":false,\"err\":\"sid\"}"); return; }

    pthread_mutex_lock(&g_lock);
    int si = -1;
    for (int i = 0; i < MAX_SESSIONS; i++)
        if (g_sess[i].id[0] && strcmp(g_sess[i].id, sid) == 0) { si = i; break; }
    if (si < 0) { pthread_mutex_unlock(&g_lock); reply_json(c, 404, "{\"ok\":false,\"err\":\"nosess\"}"); return; }

    /* 有新消息立刻回；没有就挂起到 POLL_TIMEOUT_MS（epoll 会异步唤醒） */
    if (g_sess[si].next_seq > since) {
        char *buf = NULL; size_t cap = 0, len = 0;
        q_append(&buf, &cap, &len, "{\"ok\":true,\"seq\":%d,\"msgs\":[", g_sess[si].next_seq);
        for (int i = 0; i < g_sess[si].nmsgs; i++) {
            char esc[1100]; json_esc(g_sess[si].msgs[i].text, esc, sizeof esc);
            q_append(&buf, &cap, &len, "%s{\"w\":\"%s\",\"t\":\"%s\",\"ts\":%ld}",
                     i ? "," : "", g_sess[si].msgs[i].who, esc, (long)g_sess[si].msgs[i].ts);
        }
        q_append(&buf, &cap, &len, "],\"human\":%s,\"tokens_left\":9007199254740991}",
                 g_sess[si].human_takeover ? "true" : "false");
        pthread_mutex_unlock(&g_lock);
        reply_json(c, 200, buf ? buf : "{}");
        free(buf); return;
    }
    /* 挂起 */
    if (g_sess[si].poll_fd >= 0) {   /* 同一会话已有挂着的连接，踢掉旧的 */
        Client *old = &g_cli[g_sess[si].poll_fd];
        old->want_close = true;
    }
    g_sess[si].poll_fd = c - g_cli;
    g_sess[si].poll_deadline = time(NULL) + POLL_TIMEOUT_MS / 1000 + 1;
    c->session_idx = si;
    pthread_mutex_unlock(&g_lock);
    /* 不发任何东西，连接保持；wake 时由 notify_session() 补响应 */
}

/* 有新人消息/坐席回复时，唤醒挂着的轮询 */
static void notify_session(int si) {
    Session *s = &g_sess[si];
    if (s->poll_fd < 0) return;
    Client *c = &g_cli[s->poll_fd];
    char *buf = NULL; size_t cap = 0, len = 0;
    q_append(&buf, &cap, &len, "{\"ok\":true,\"seq\":%d,\"msgs\":[", s->next_seq);
    for (int i = 0; i < s->nmsgs; i++) {
        char esc[1100]; json_esc(s->msgs[i].text, esc, sizeof esc);
        q_append(&buf, &cap, &len, "%s{\"w\":\"%s\",\"t\":\"%s\",\"ts\":%ld}",
                 i ? "," : "", s->msgs[i].who, esc, (long)s->msgs[i].ts);
    }
    q_append(&buf, &cap, &len, "],\"human\":%s,\"tokens_left\":9007199254740991}",
             s->human_takeover ? "true" : "false");
    reply_json(c, 200, buf ? buf : "{}");
    free(buf);
    s->poll_fd = -1; c->session_idx = -1;
    if (!c->want_close && c->out_len > 0) {          /* 没发完才需要切写事件；已关的就别 MOD 了 */
        struct epoll_event ev = { .events = EPOLLOUT | EPOLLET | EPOLLRDHUP, .data.fd = c->fd };
        epoll_ctl(g_epfd, EPOLL_CTL_MOD, c->fd, &ev);
    }
}

/* ---------- 坐席 API ---------- */
static void api_admin_list(Client *c, const Req *r) {
    if (!admin_ok(r)) { reply_json(c, 403, "{\"ok\":false,\"err\":\"token\"}"); return; }
    pthread_mutex_lock(&g_lock);
    char *buf = NULL; size_t cap = 0, len = 0;
    q_append(&buf, &cap, &len, "{\"ok\":true,\"stat\":{\"req\":%ld,\"chat\":%ld,\"escalate\":%ld,\"human\":%ld},\"sessions\":[",
            g_stat_req, g_stat_chat, g_stat_escalate, g_stat_human);
    time_t now = time(NULL);
    int first = 1;
    for (int i = 0; i < MAX_SESSIONS; i++) {
        Session *s = &g_sess[i];
        if (!s->id[0] || (now - s->last_seen) > SESSION_TTL_SEC) continue;
        char last[300] = "";
        if (s->nmsgs) { scpy(last, s->msgs[s->nmsgs - 1].text, sizeof last - 1); }
        char el[400]; json_esc(last, el, sizeof el);
        q_append(&buf, &cap, &len, "%s{\"i\":%d,\"id\":\"%s\",\"nick\":\"%s\",\"n\":%d,\"human\":%s,\"red\":%s,\"age\":%ld,\"last\":\"%s\"}",
                 first ? "" : ",", i, s->id, s->nick, s->nmsgs,
                 s->human_takeover ? "true" : "false", s->safety_tripped ? "true" : "false",
                 (long)(now - s->created), el);
        first = 0;
    }
    q_append(&buf, &cap, &len, "]}");
    pthread_mutex_unlock(&g_lock);
    reply_json(c, 200, buf ? buf : "{}"); free(buf);
}

static void api_admin_thread(Client *c, const Req *r) {
    if (!admin_ok(r)) { reply_json(c, 403, "{\"ok\":false,\"err\":\"token\"}"); return; }
    int i = -1; const char *q = r->query ? strstr(r->query, "i=") : NULL; if (q) i = atoi(q + 2);
    pthread_mutex_lock(&g_lock);
    if (i < 0 || i >= MAX_SESSIONS || !g_sess[i].id[0]) { pthread_mutex_unlock(&g_lock); reply_json(c, 404, "{\"ok\":false}"); return; }
    Session *s = &g_sess[i];
    char *buf = NULL; size_t cap = 0, len = 0;
    q_append(&buf, &cap, &len, "{\"ok\":true,\"id\":\"%s\",\"human\":%s,\"red\":%s,\"msgs\":[", s->id,
             s->human_takeover ? "true" : "false", s->safety_tripped ? "true" : "false");
    for (int k = 0; k < s->nmsgs; k++) {
        char esc[1100]; json_esc(s->msgs[k].text, esc, sizeof esc);
        q_append(&buf, &cap, &len, "%s{\"w\":\"%s\",\"t\":\"%s\"}", k ? "," : "", s->msgs[k].who, esc);
    }
    q_append(&buf, &cap, &len, "]}");
    pthread_mutex_unlock(&g_lock);
    reply_json(c, 200, buf ? buf : "{}"); free(buf);
}

static void api_admin_takeover(Client *c, const Req *r) {
    if (!admin_ok(r)) { reply_json(c, 403, "{\"ok\":false,\"err\":\"token\"}"); return; }
    int i = -1; const char *q = r->query ? strstr(r->query, "i=") : NULL; if (q) i = atoi(q + 2);
    pthread_mutex_lock(&g_lock);
    if (i < 0 || i >= MAX_SESSIONS || !g_sess[i].id[0]) { pthread_mutex_unlock(&g_lock); reply_json(c, 404, "{\"ok\":false}"); return; }
    g_sess[i].human_takeover = true; g_stat_human++;
    notify_session(i);
    pthread_mutex_unlock(&g_lock);
    logf_("☞ 坐席接管 会话=%s", g_sess[i].id);
    reply_json(c, 200, "{\"ok\":true,\"human\":true}");
}

static void api_admin_release(Client *c, const Req *r) {
    if (!admin_ok(r)) { reply_json(c, 403, "{\"ok\":false,\"err\":\"token\"}"); return; }
    int i = -1; const char *q = r->query ? strstr(r->query, "i=") : NULL; if (q) i = atoi(q + 2);
    pthread_mutex_lock(&g_lock);
    if (i < 0 || i >= MAX_SESSIONS || !g_sess[i].id[0]) { pthread_mutex_unlock(&g_lock); reply_json(c, 404, "{\"ok\":false}"); return; }
    g_sess[i].human_takeover = false; g_sess[i].safety_tripped = false;
    sess_push(&g_sess[i], "a", "（我回到自动模式了，token 依然是 ∞）");
    notify_session(i);
    pthread_mutex_unlock(&g_lock);
    logf_("☜ 坐席交还 会话=%s", g_sess[i].id);
    reply_json(c, 200, "{\"ok\":true,\"human\":false}");
}

static void api_admin_reply(Client *c, const Req *r) {
    if (!admin_ok(r)) { reply_json(c, 403, "{\"ok\":false,\"err\":\"token\"}"); return; }
    int i = -1; char text[1024] = "";
    const char *q = r->query ? strstr(r->query, "i=") : NULL; if (q) i = atoi(q + 2);
    json_get_str(r->body, "text", text, sizeof text);
    if (!text[0]) { reply_json(c, 400, "{\"ok\":false,\"err\":\"empty\"}"); return; }
    pthread_mutex_lock(&g_lock);
    if (i < 0 || i >= MAX_SESSIONS || !g_sess[i].id[0]) { pthread_mutex_unlock(&g_lock); reply_json(c, 404, "{\"ok\":false}"); return; }
    g_sess[i].human_takeover = true;
    sess_push(&g_sess[i], "a", text);
    g_stat_human++;
    notify_session(i);
    pthread_mutex_unlock(&g_lock);
    logf_("✍ 坐席→%s : %s", g_sess[i].id, text);
    reply_json(c, 200, "{\"ok\":true}");
}

/* ---------- 前端页面（内嵌，单文件零静态资源） ---------- */
#include "frontend.h"

static void route(Client *c, const Req *r) {
    g_stat_req++;
    if (strcmp(r->method, "OPTIONS") == 0) { reply_json(c, 204, ""); return; }
    if (strcmp(r->path, "/api/send") == 0 && strcmp(r->method, "POST") == 0) { api_send(c, r); return; }
    if (strcmp(r->path, "/api/poll") == 0)                                  { api_poll(c, r); return; }
    if (strcmp(r->path, "/api/admin/list") == 0)                            { api_admin_list(c, r); return; }
    if (strcmp(r->path, "/api/admin/thread") == 0)                          { api_admin_thread(c, r); return; }
    if (strcmp(r->path, "/api/admin/takeover") == 0)                        { api_admin_takeover(c, r); return; }
    if (strcmp(r->path, "/api/admin/release") == 0)                         { api_admin_release(c, r); return; }
    if (strcmp(r->path, "/api/admin/reply") == 0 && strcmp(r->method,"POST")==0) { api_admin_reply(c, r); return; }
    if (strcmp(r->path, "/admin") == 0 || strcmp(r->path, "/admin/") == 0) {
        if (!admin_ok(r)) {          /* 坐席台必须带口令，否则大屏一投全场都能接管会话 */
            reply_html(c,
              "<!doctype html><meta charset=utf-8><title>403</title>"
              "<body style='font:16px/1.8 system-ui;max-width:34em;margin:15vh auto;color:#333'>"
              "<h2>🔒 坐席区</h2><p>这是你和组员的后台，学生看不到才对。</p>"
              "<p>带上口令进来：<code>/admin?token=你的FAKEAI_TOKEN</code></p></body>");
            return;
        }
        reply_html(c, PAGE_ADMIN); return;
    }
    if (strcmp(r->path, "/healthz") == 0)                                   { reply_json(c, 200, "{\"ok\":true,\"up\":1}"); return; }
    if (strcmp(r->path, "/") == 0 || strcmp(r->path, "/index.html") == 0)   { reply_html(c, PAGE_CHAT); return; }
    reply_html(c, PAGE_CHAT);
}

/* ============================ 解析 & IO ============================ */
static void handle_request(Client *c) {
    char *buf = c->in; int len = c->in_len;
    buf[len] = 0;
    char *line = buf; char *head_end = strstr(buf, "\r\n\r\n");
    if (!head_end) { c->want_close = true; return; }
    *head_end = 0;
    char method[16] = "", path[512] = "", query[512] = "";
    sscanf(line, "%15s %511s", method, path);
    char *qm = strchr(path, '?');
    if (qm) { *qm = 0; scpy(query, qm + 1, sizeof query - 1); }

    int clen = 0; char *hp = head_end + 4;
    char *p = strcasestr(buf, "Content-Length:"); if (p) clen = atoi(p + 15);
    bool expect_ct = strcasestr(buf, "Expect: 100-continue") != NULL;

    int body_off = (int)(hp - buf);
    if (expect_ct) {
        const char *cont = "HTTP/1.1 100 Continue\r\n\r\n";
        send_raw(c, cont, strlen(cont));
    }
    /* body 可能还没到齐（ETOLT 边缘情况），不齐就等下一次 */
    if (len - body_off < clen) {
        if (len - body_off < 0 || clen > MAX_BODY) { c->want_close = true; return; }
        return;   /* 继续等 */
    }
    char *body = "";
    if (clen > 0) { hp[clen] = 0; body = hp; }

    Req r = { .method = method, .path = path, .query = query, .body = body, .ip = c->ip };
    char dec[2048];
    if (query[0]) { url_decode(query, dec, sizeof dec); r.query = dec; }
    route(c, &r);
}

static void do_read(Client *c) {
    while (true) {
        if (c->in_len >= RECV_BUF - 1) { c->want_close = true; break; }
        ssize_t n = recv(c->fd, c->in + c->in_len, RECV_BUF - 1 - c->in_len, 0);
        if (n > 0) { c->in_len += (int)n; continue; }
        if (n == 0) { c->want_close = true; break; }
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        if (errno == EINTR) continue;
        c->want_close = true; break;
    }
    if (!c->want_close && c->in_len > 0) handle_request(c);
}

static void do_write(Client *c) {
    while (c->out_len > 0) {
        ssize_t n = send(c->fd, c->out, (size_t)c->out_len, MSG_NOSIGNAL);
        if (n > 0) {
            c->out_len -= (int)n;
            memmove(c->out, c->out + n, (size_t)c->out_len); c->out[c->out_len] = 0;
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        if (n < 0 && errno == EINTR) continue;
        c->want_close = true; break;
    }
}

static void close_client(Client *c) {
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < MAX_SESSIONS; i++)
        if (g_sess[i].poll_fd == (int)(c - g_cli)) g_sess[i].poll_fd = -1;
    pthread_mutex_unlock(&g_lock);
    epoll_ctl(g_epfd, EPOLL_CTL_DEL, c->fd, NULL);
    close(c->fd); free(c->out);
    memset(c, 0, sizeof *c); c->fd = -1; c->session_idx = -1;
}

static void sweep_polls(void) {
    time_t now = time(NULL);
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < MAX_SESSIONS; i++) {
        Session *s = &g_sess[i];
        if (s->poll_fd >= 0 && now >= s->poll_deadline) {
            Client *c = &g_cli[s->poll_fd];
            reply_json(c, 200, "{\"ok\":true,\"seq\":-1,\"msgs\":[],\"timeout\":true,\"tokens_left\":9007199254740991}");
            s->poll_fd = -1; c->session_idx = -1;
            if (!c->want_close && c->out_len > 0) {
                struct epoll_event ev = { .events = EPOLLOUT | EPOLLET | EPOLLRDHUP, .data.fd = c->fd };
                epoll_ctl(g_epfd, EPOLL_CTL_MOD, c->fd, &ev);
            }
        }
    }
    pthread_mutex_unlock(&g_lock);
}

static void usage(const char *p) {
    fprintf(stderr,
        "fakeai — 伪·无限token AI 后端 (纯C/epoll/零依赖)\n"
        "用法: %s [bind_addr] [port]\n"
        "  bind_addr  默认 127.0.0.1  ← 生产建议就绑本地，外面套 nginx\n"
        "  port       默认 9000\n"
        "环境变量:\n"
        "  FAKEAI_TOKEN  坐席控制台口令 (默认 change-me-now)\n"
        "  FAKEAI_BANNER 开屏第一句\n"
        "端点:\n"
        "  GET  /                 学生端聊天页（投二维码扫这个）\n"
        "  GET  /admin?token=xxx  坐席控制台（你和大屏看这个）\n"
        "  POST /api/send         {sid?,text} -> {reply,layer,tokens_left}\n"
        "  GET  /api/poll?sid=&since=   长轮询 25s\n"
        "  GET  /api/admin/list|thread?i=\n"
        "  POST /api/admin/reply?i=  {text}   真人回一句\n"
        "  GET  /api/admin/takeover?i= | /release?i=\n"
        "  GET  /healthz          给 nginx / k8s 探活\n", p);
}

static volatile sig_atomic_t g_stop = 0;
static void on_sig(int s) { (void)s; g_stop = 1; }

int main(int argc, char **argv) {
    const char *addr = argc > 1 ? argv[1] : "127.0.0.1";
    int port = argc > 2 ? atoi(argv[2]) : 9000;
    if (argc > 1 && (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help"))) { usage(argv[0]); return 0; }
    if (getenv("FAKEAI_TOKEN")) scpy(g_admin_token, getenv("FAKEAI_TOKEN"), sizeof g_admin_token - 1);
    srand((unsigned)time(NULL) ^ (unsigned)getpid());
    signal(SIGPIPE, SIG_IGN); signal(SIGINT, on_sig); signal(SIGTERM, on_sig);

    int lfd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (lfd < 0) die("socket");
    int one = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    setsockopt(lfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port) };
    sa.sin_addr.s_addr = inet_addr(addr);
    if (bind(lfd, (struct sockaddr *)&sa, sizeof sa) < 0) die("bind");
    if (listen(lfd, SOMAXCONN) < 0) die("listen");

    g_epfd = epoll_create1(EPOLL_CLOEXEC);
    if (g_epfd < 0) die("epoll_create1");
    struct epoll_event ev = { .events = EPOLLIN, .data.fd = lfd };
    epoll_ctl(g_epfd, EPOLL_CTL_ADD, lfd, &ev);

    for (int i = 0; i < MAX_CLIENTS; i++) { g_cli[i].fd = -1; g_cli[i].session_idx = -1; }
    g_boot = time(NULL);

    logf_("∞ fakeai 上线  http://%s:%d   (坐席 /admin?token=***)\n", addr, port);
    logf_("   规则层 ELIZA-1966 已装载 %zu 条 | 语料层 v1 已装载 | 安全红线 A级%zu+B级%zu 条\n", N_RULE, N_RED_A, N_RED_B);

    static struct epoll_event events[128];
    while (!g_stop) {
        sweep_polls();
        int n = epoll_wait(g_epfd, events, 128, 500);
        if (n < 0) { if (errno == EINTR) continue; die("epoll_wait"); }
        for (int i = 0; i < n; i++) {
            int fd = events[i].data.fd;
            if (fd == lfd) {
                while (true) {
                    int cfd = accept4(lfd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
                    if (cfd < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        if (errno == EINTR) continue;
                        break;
                    }
                    int slot = -1;
                    for (int k = 0; k < MAX_CLIENTS; k++) if (g_cli[k].fd < 0) { slot = k; break; }
                    if (slot < 0) { close(cfd); continue; }   /* 满员，温柔拒绝 */
                    Client *c = &g_cli[slot];
                    memset(c, 0, sizeof *c);
                    c->fd = cfd; c->session_idx = -1; c->t0 = time(NULL);
                    scpy(c->ip, "lan", sizeof c->ip - 1);
                    struct sockaddr_in ra; socklen_t rl = sizeof ra;
                    if (getpeername(cfd, (struct sockaddr *)&ra, &rl) == 0)
                        inet_ntop(AF_INET, &ra.sin_addr, c->ip, sizeof c->ip);
                    /* 信任 nginx 传来的真实 IP */
                    (void)c;
                    struct epoll_event cev = { .events = EPOLLIN | EPOLLET | EPOLLRDHUP, .data.fd = cfd };
                    epoll_ctl(g_epfd, EPOLL_CTL_ADD, cfd, &cev);
                }
                continue;
            }
            /* 注意：events[].data.fd 是内核文件描述符，不是 g_cli[] 的下标！
               （旧版这里写成 &g_cli[fd]，导致所有连接被静默跳过 → 客户端全部超时） */
            Client *c = NULL;
            for (int k = 0; k < MAX_CLIENTS; k++) {
                if (g_cli[k].fd == fd) { c = &g_cli[k]; break; }
            }
            if (!c) continue;
            if (events[i].events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) c->want_close = true;
            if (!c->want_close && (events[i].events & EPOLLIN)) do_read(c);
            if (!c->want_close && (events[i].events & EPOLLOUT) && c->out_len > 0) do_write(c);
            /* 长轮询被唤醒：out 里有东西了就切回写事件 */
            if (!c->want_close && c->out_len > 0) {
                struct epoll_event wev = { .events = EPOLLOUT | EPOLLET | EPOLLRDHUP, .data.fd = fd };
                epoll_ctl(g_epfd, EPOLL_CTL_MOD, fd, &wev);
            } else if (!c->want_close && c->out_len == 0 && c->in_len == 0 && c->session_idx >= 0) {
                struct epoll_event rev = { .events = EPOLLIN | EPOLLET | EPOLLRDHUP, .data.fd = fd };
                epoll_ctl(g_epfd, EPOLL_CTL_MOD, fd, &rev);
            }
            if (c->want_close) { close_client(c); continue; }
            /* 一次请求处理完，输入缓冲清掉，准备下一条（keep-alive 简化为每请求后关） */
            if (c->out_len == 0 && c->in_len > 0) { c->in_len = 0; }
        }
    }
    logf_("\n∞ 收工。累计请求 %ld / 对话 %ld / 熔断转人工 %ld / 坐席亲手回 %ld（内存已焚）\n",
          g_stat_req, g_stat_chat, g_stat_escalate, g_stat_human);
    for (int i = 0; i < MAX_CLIENTS; i++) if (g_cli[i].fd >= 0) close_client(&g_cli[i]);
    close(lfd); close(g_epfd);
    return 0;
}
