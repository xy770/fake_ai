package main

import (
	"regexp"
	"strconv"
	"strings"
	"unicode/utf8"
)

var helloRe = regexp.MustCompile(`^(你好|您好|嗨|哈喽|hello|hi|在吗|在不在)`)

var capRe = regexp.MustCompile(`^(你会|你能|你可以|你懂|你会不会|你能不能|你知不知道|会|能).{0,16}(吗|么|？|\?)$`)

var questionWords = []string{"为什么", "怎么", "什么", "哪", "谁", "多少", "是否", "吗", "呢"}

var flipPairs = []struct{ from, to string }{
	{"我自己", "你自己"},
	{"你自己", "我自己"},
	{"我们", "你们"},
	{"你们", "我们"},
	{"我的", "你的"},
	{"你的", "我的"},
	{"我", "你"},
	{"你", "我"},
}

func flipPronouns(s string) string {
	var b strings.Builder
	for i := 0; i < len(s); {
		matched := false
		for _, p := range flipPairs {
			if strings.HasPrefix(s[i:], p.from) {
				b.WriteString(p.to)
				i += len(p.from)
				matched = true
				break
			}
		}
		if matched {
			continue
		}
		_, size := utf8.DecodeRuneInString(s[i:])
		b.WriteString(s[i : i+size])
		i += size
	}
	return b.String()
}

func isQuestion(s string) bool {
	if strings.ContainsAny(s, "？?") {
		return true
	}
	for _, w := range questionWords {
		if strings.Contains(s, w) {
			return true
		}
	}
	return false
}

func countIn(ring []string, msg string) int {
	n := 0
	for _, e := range ring {
		if e == msg {
			n++
		}
	}
	return n
}

func scoreMsg(c *compiled, m string) int {
	total := 0
	for word, sc := range c.cfg.Lexicon {
		if strings.Contains(m, word) {
			total += sc
		}
	}
	return total
}

func absInt(x int) int {
	if x < 0 {
		return -x
	}
	return x
}

func pick(turn int, ts []string) string {
	if len(ts) == 0 {
		return ""
	}
	return ts[turn%len(ts)]
}

func fill(c *compiled, t, kw, flip, old, n string) string {
	flip = strings.TrimRight(flip, "？?。！! 、，, ")
	r := strings.NewReplacer(
		"{kw}", kw,
		"{flip}", flip,
		"{old}", old,
		"{n}", n,
		"{room}", c.cfg.Room,
	)
	return r.Replace(t)
}

func pickMemory(c *compiled, s *Session, msg string) string {
	best := ""
	bestVal := -1
	for i, e := range s.ring {
		if s.echoed[e] || e == msg || isQuestion(e) {
			continue
		}
		r := utf8.RuneCountInString(e)
		if r < 4 {
			continue
		}
		es := scoreMsg(c, e)
		if es == 0 && r < 8 {
			continue
		}
		v := absInt(es)*2 + i
		if v > bestVal {
			bestVal = v
			best = e
		}
	}
	return best
}

func heaviest(c *compiled, s *Session) string {
	best := ""
	bestScore := 0
	bestLen := 0
	for i, e := range s.ring {
		sc := scoreMsg(c, e)
		r := utf8.RuneCountInString(e)
		if i == 0 || sc < bestScore || (sc == bestScore && r > bestLen) {
			bestScore = sc
			bestLen = r
			best = e
		}
	}
	return best
}

func withMood(c *compiled, s *Session, score int, out string) string {
	if score < 0 {
		s.negStreak++
	} else if score > 0 {
		if s.lastScore < 0 {
			out += "\n" + pick(s.turn, c.pool("lift"))
		}
		s.negStreak = 0
	}
	if s.negStreak == 3 {
		out += "\n" + pick(s.turn, c.pool("care"))
	}
	s.lastScore = score
	return out
}

func Reply(msg string, s *Session) (string, string) {
	c := current()
	msg = strings.TrimSpace(msg)
	if msg == "" {
		return "说点什么吧，哪怕一个字也行。", "EMPTY / 没接到词"
	}
	score := scoreMsg(c, msg)
	if helloRe.MatchString(strings.ToLower(msg)) {
		return pick(s.turn, c.pool("hello")), "TEMPLATE / 招呼"
	}
	if n := countIn(s.ring, msg); n >= 1 {
		t := fill(c, pick(s.turn, c.pool("repeat")), "", "", msg, strconv.Itoa(n+1))
		return withMood(c, s, score, t), "REPEAT / 第 " + strconv.Itoa(n+1) + " 遍"
	}
	for _, r := range c.rules {
		if len(r.templates) == 0 && r.name != c.cfg.ByeRule {
			continue
		}
		if kw := r.re.FindString(msg); kw != "" {
			tag := "REGEX / " + r.name + " → 「" + kw + "」"
			if r.name == c.cfg.ByeRule {
				if old := heaviest(c, s); old != "" {
					t := fill(c, pick(s.turn, c.pool("bye_summary")), kw, "", old, strconv.Itoa(s.turn+1))
					return withMood(c, s, score, t), tag + " + SUMMARY"
				}
				return withMood(c, s, score, pick(s.turn, c.pool("bye"))), tag
			}
			tpl := pick(s.turn, r.templates)
			if strings.Contains(tpl, "{flip}") {
				tag += " + FLIP"
			}
			out := fill(c, tpl, kw, flipPronouns(msg), "", "")
			if s.turn >= 2 && s.turn%4 == 2 {
				if cand := pickMemory(c, s, msg); cand != "" {
					s.echoed[cand] = true
					out += "\n" + fill(c, pick(s.turn, c.pool("memory_suffix")), "", "", cand, "")
					tag += " + RING"
				}
			}
			return withMood(c, s, score, out), tag
		}
	}
	if capRe.MatchString(msg) {
		t := fill(c, pick(s.turn, c.pool("capability")), "", "", "", "")
		return withMood(c, s, score, t), "CAPABILITY / 技能栏自曝"
	}
	if cand := pickMemory(c, s, msg); cand != "" {
		s.echoed[cand] = true
		t := fill(c, pick(s.turn, c.pool("memory")), "", "", cand, "")
		return withMood(c, s, score, t), "RING / 旧话重提（挑过的）"
	}
	if isQuestion(msg) {
		t := fill(c, pick(s.turn, c.pool("question")), "", flipPronouns(msg), "", "")
		return withMood(c, s, score, t), "QUESTION / 反问"
	}
	flip := flipPronouns(msg)
	if flip != msg {
		t := fill(c, pick(s.turn, c.pool("flip")), "", flip, "", "")
		return withMood(c, s, score, t), "PRONOUN FLIP / 我↔你"
	}
	return withMood(c, s, score, pick(s.turn, c.pool("fallback"))), "TEMPLATE / 兜底池"
}
