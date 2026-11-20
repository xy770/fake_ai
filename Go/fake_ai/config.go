package main

import (
	_ "embed"
	"encoding/json"
	"fmt"
	"os"
	"regexp"
	"sync"
)

//go:embed config.json
var defaultConfigBytes []byte

type RuleSpec struct {
	Name      string   `json:"name"`
	Re        string   `json:"re"`
	Templates []string `json:"templates"`
}

type Config struct {
	Room    string              `json:"room"`
	ByeRule string              `json:"bye_rule"`
	Rules   []RuleSpec          `json:"rules"`
	Pools   map[string][]string `json:"pools"`
	Lexicon map[string]int      `json:"lexicon"`
}

type rule struct {
	name      string
	re        *regexp.Regexp
	templates []string
}

type compiled struct {
	cfg   Config
	rules []rule
}

var (
	cfgMu sync.RWMutex
	cur   *compiled
)

func current() *compiled {
	cfgMu.RLock()
	defer cfgMu.RUnlock()
	return cur
}

func setCur(c *compiled) {
	cfgMu.Lock()
	cur = c
	cfgMu.Unlock()
}

func (c *compiled) pool(name string) []string {
	return c.cfg.Pools[name]
}

func loadCompiled(path string) (*compiled, error) {
	var def Config
	if err := json.Unmarshal(defaultConfigBytes, &def); err != nil {
		return nil, fmt.Errorf("embedded config: %w", err)
	}
	cfg := def
	if path != "" {
		b, err := os.ReadFile(path)
		if err != nil && !os.IsNotExist(err) {
			return nil, err
		}
		if err == nil {
			var ext Config
			if err := json.Unmarshal(b, &ext); err != nil {
				return nil, fmt.Errorf("parse %s: %w", path, err)
			}
			if ext.Room != "" {
				cfg.Room = ext.Room
			}
			if ext.ByeRule != "" {
				cfg.ByeRule = ext.ByeRule
			}
			if len(ext.Rules) > 0 {
				cfg.Rules = ext.Rules
			}
			if len(ext.Pools) > 0 {
				merged := map[string][]string{}
				for k, v := range def.Pools {
					merged[k] = v
				}
				for k, v := range ext.Pools {
					if len(v) > 0 {
						merged[k] = v
					}
				}
				cfg.Pools = merged
			}
			if len(ext.Lexicon) > 0 {
				merged := map[string]int{}
				for k, v := range def.Lexicon {
					merged[k] = v
				}
				for k, v := range ext.Lexicon {
					merged[k] = v
				}
				cfg.Lexicon = merged
			}
		}
	}
	c := &compiled{cfg: cfg}
	for _, rs := range cfg.Rules {
		re, err := regexp.Compile(rs.Re)
		if err != nil {
			return nil, fmt.Errorf("rule %s: %w", rs.Name, err)
		}
		c.rules = append(c.rules, rule{name: rs.Name, re: re, templates: rs.Templates})
	}
	return c, nil
}
