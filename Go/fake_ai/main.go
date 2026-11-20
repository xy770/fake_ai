package main

import (
	"crypto/rand"
	"embed"
	"encoding/hex"
	"encoding/json"
	"flag"
	"fmt"
	"io/fs"
	"log"
	"net/http"
	"os"
	"os/signal"
	"strings"
	"sync"
	"syscall"
	"time"
)

//go:embed web
var webFS embed.FS

const (
	ringCap    = 6
	sessionTTL = 2 * time.Hour
	cookieName = "fakeai_sid"
)

type Session struct {
	ring       []string
	turn       int
	lastActive time.Time
	negStreak  int
	lastScore  int
	echoed     map[string]bool
}

var (
	mu       sync.Mutex
	sessions = map[string]*Session{}
)

func newID() string {
	b := make([]byte, 16)
	if _, err := rand.Read(b); err != nil {
		return fmt.Sprintf("%d", time.Now().UnixNano())
	}
	return hex.EncodeToString(b)
}

func reap() {
	t := time.NewTicker(10 * time.Minute)
	for range t.C {
		mu.Lock()
		now := time.Now()
		for id, s := range sessions {
			if now.Sub(s.lastActive) > sessionTTL {
				delete(sessions, id)
			}
		}
		mu.Unlock()
	}
}

func handleChat(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	var req struct {
		Message string `json:"message"`
	}
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		http.Error(w, "bad json", http.StatusBadRequest)
		return
	}
	sid := ""
	if c, err := r.Cookie(cookieName); err == nil && c.Value != "" {
		sid = c.Value
	} else {
		sid = newID()
		http.SetCookie(w, &http.Cookie{
			Name:     cookieName,
			Value:    sid,
			Path:     "/",
			MaxAge:   int(sessionTTL.Seconds()),
			HttpOnly: true,
			SameSite: http.SameSiteLaxMode,
		})
	}
	mu.Lock()
	s, ok := sessions[sid]
	if !ok {
		s = &Session{echoed: map[string]bool{}}
		sessions[sid] = s
	}
	s.lastActive = time.Now()
	reply, tag := Reply(req.Message, s)
	if msg := strings.TrimSpace(req.Message); msg != "" {
		s.ring = append(s.ring, msg)
		if len(s.ring) > ringCap {
			s.ring = s.ring[1:]
		}
	}
	s.turn++
	turn := s.turn
	ringLen := len(s.ring)
	mu.Unlock()
	w.Header().Set("Content-Type", "application/json; charset=utf-8")
	_ = json.NewEncoder(w).Encode(map[string]any{
		"reply": reply,
		"tag":   tag,
		"turn":  turn,
		"ring":  ringLen,
	})
}

func handleHealth(w http.ResponseWriter, r *http.Request) {
	mu.Lock()
	n := len(sessions)
	mu.Unlock()
	w.Header().Set("Content-Type", "application/json; charset=utf-8")
	_ = json.NewEncoder(w).Encode(map[string]any{"ok": true, "sessions": n})
}

func main() {
	addr := flag.String("addr", "127.0.0.1:8787", "listen address")
	cfgPath := flag.String("config", "rules.json", "external config path, falls back to embedded config.json when missing")
	flag.Parse()
	c, err := loadCompiled(*cfgPath)
	if err != nil {
		log.Fatal(err)
	}
	setCur(c)
	hup := make(chan os.Signal, 1)
	signal.Notify(hup, syscall.SIGHUP)
	go func() {
		for range hup {
			nc, err := loadCompiled(*cfgPath)
			if err != nil {
				log.Printf("config reload failed, keeping old: %v", err)
				continue
			}
			setCur(nc)
			log.Printf("config reloaded from %s", *cfgPath)
		}
	}()
	go reap()
	sub, err := fs.Sub(webFS, "web")
	if err != nil {
		log.Fatal(err)
	}
	mux := http.NewServeMux()
	mux.HandleFunc("/api/chat", handleChat)
	mux.HandleFunc("/api/health", handleHealth)
	mux.Handle("/", http.FileServer(http.FS(sub)))
	log.Printf("fake_ai listening on %s", *addr)
	log.Fatal(http.ListenAndServe(*addr, mux))
}
