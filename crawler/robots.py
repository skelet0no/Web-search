import re
from urllib.parse import urlsplit, quote
from urllib.robotparser import RobotFileParser


class RobotsRules(RobotFileParser):
    def parse(self, lines):
        super().parse(lines)
        self.rule_groups = []
        agents, rules = [], []
        seen_directive = False
        for raw in lines:
            line = raw.split("#", 1)[0].strip()
            if ":" not in line:
                continue
            key, value = [part.strip() for part in line.split(":", 1)]
            key = key.lower()
            if key == "user-agent":
                if seen_directive:
                    self.rule_groups.append((agents, rules))
                    agents, rules, seen_directive = [], [], False
                agents.append(value.lower())
            elif agents and key in ("allow", "disallow", "crawl-delay", "request-rate"):
                seen_directive = True
                if key in ("allow", "disallow") and value:
                    rules.append((value, key == "allow"))
        if agents:
            self.rule_groups.append((agents, rules))

    def can_fetch(self, useragent, url):
        agent = useragent.lower()
        candidates = [(max((len(a) if a != "*" else 0) for a in agents if a == "*" or a in agent), rules)
                      for agents, rules in self.rule_groups if any(a == "*" or a in agent for a in agents)]
        if not candidates:
            return True
        specificity = max(score for score, _ in candidates)
        parts = urlsplit(url)
        path = quote(parts.path or "/", safe="/%:@!$&'()*+,;=-._~")
        if parts.query:
            path += "?" + parts.query
        matches = []
        for score, rules in candidates:
            if score != specificity:
                continue
            for pattern, allowed in rules:
                end = pattern.endswith("$")
                body = pattern[:-1] if end else pattern
                regex = "^" + ".*".join(re.escape(p) for p in body.split("*")) + ("$" if end else "")
                if re.search(regex, path):
                    matches.append((len(body.replace("*", "")), allowed))
        return max(matches, default=(0, True))[1]
