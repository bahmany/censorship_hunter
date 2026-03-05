import logging
import os
from typing import Any, Dict, List, Optional

try:
    import requests
except ImportError:
    requests = None


class OllamaClient:
    def __init__(
        self,
        base_url: Optional[str] = None,
        default_model: Optional[str] = None,
        timeout_s: float = 25.0,
    ):
        self.logger = logging.getLogger(__name__)
        raw_url = (base_url or os.getenv("HUNTER_OLLAMA_URL") or os.getenv("OLLAMA_HOST") or "http://127.0.0.1:11434").strip()
        if raw_url and not raw_url.startswith("http://") and not raw_url.startswith("https://"):
            raw_url = "http://" + raw_url
        self.base_url = raw_url.rstrip("/")
        self.default_model = default_model or os.getenv("HUNTER_OLLAMA_MODEL") or os.getenv("OLLAMA_MODEL")
        self.timeout_s = float(timeout_s)
        self._cached_model: Optional[str] = None

    def _session(self):
        if requests is None:
            return None
        s = requests.Session()
        s.trust_env = False
        return s

    def is_available(self) -> bool:
        s = self._session()
        if s is None:
            return False
        try:
            r = s.get(f"{self.base_url}/api/tags", timeout=min(self.timeout_s, 2.0))
            return bool(r.ok)
        except Exception:
            return False

    def list_models(self) -> List[str]:
        s = self._session()
        if s is None:
            return []
        try:
            r = s.get(f"{self.base_url}/api/tags", timeout=min(self.timeout_s, 2.0))
            if not r.ok:
                return []
            data = r.json() or {}
            models = data.get("models") or []
            out: List[str] = []
            for m in models:
                name = (m or {}).get("name")
                if isinstance(name, str) and name:
                    out.append(name)
            return out
        except Exception:
            return []

    def pick_model(self) -> Optional[str]:
        if self.default_model:
            return self.default_model
        if self._cached_model:
            return self._cached_model
        models = self.list_models()
        if not models:
            return None

        priority = [
            "qwen2.5",
            "qwen2",
            "llama3.3",
            "llama3.2",
            "llama3.1",
            "llama3",
            "mistral",
            "mixtral",
            "phi3",
            "gemma2",
            "gemma",
        ]
        lowered = [(m, m.lower()) for m in models]
        for key in priority:
            for m, ml in lowered:
                if key in ml:
                    self._cached_model = m
                    return m

        self._cached_model = models[0]
        return models[0]

    def chat(self, messages: List[Dict[str, str]], model: Optional[str] = None) -> Optional[str]:
        s = self._session()
        if s is None:
            return None
        model = model or self.pick_model()
        if not model:
            return None
        payload: Dict[str, Any] = {"model": model, "messages": messages, "stream": False}
        try:
            r = s.post(f"{self.base_url}/api/chat", json=payload, timeout=self.timeout_s)
            if r.status_code == 404:
                return self.generate("\n".join([m.get("content", "") for m in messages]), model=model)
            if not r.ok:
                return None
            data = r.json() or {}
            msg = (data.get("message") or {}).get("content")
            if isinstance(msg, str) and msg.strip():
                return msg.strip()
            return None
        except Exception:
            return None

    def generate(self, prompt: str, model: Optional[str] = None) -> Optional[str]:
        s = self._session()
        if s is None:
            return None
        model = model or self.pick_model()
        if not model:
            return None
        payload = {"model": model, "prompt": prompt, "stream": False}
        try:
            r = s.post(f"{self.base_url}/api/generate", json=payload, timeout=self.timeout_s)
            if not r.ok:
                return None
            data = r.json() or {}
            text = data.get("response")
            if isinstance(text, str) and text.strip():
                return text.strip()
            return None
        except Exception:
            return None

    def summarize_configs(self, configs: List[Dict[str, Any]], lang: str = "fa") -> Optional[str]:
        if not configs:
            return None
        if os.getenv("HUNTER_OLLAMA_ENABLED", "true").lower() != "true":
            return None
        if not self.is_available():
            return None

        top = configs[:10]
        lines = []
        for c in top:
            ps = c.get("ps", "")
            lat = c.get("latency_ms", 0)
            tier = c.get("tier", "")
            if not isinstance(ps, str):
                ps = ""
            try:
                lat_f = float(lat)
            except Exception:
                lat_f = 0.0
            lines.append(f"- {tier} | {lat_f:.0f}ms | {ps}"[:160])

        sys = "".join([
            "You are a concise assistant for a VPN config hunting tool. ",
            "Summarize the current batch for the operator in 2-4 short lines. ",
            "Avoid markdown syntax characters like *, _, `.",
        ])
        if lang.lower().startswith("fa"):
            sys += " Respond in Persian (Farsi)."
        else:
            sys += " Respond in English."

        user = "Here are the top validated configs:\n" + "\n".join(lines)
        msg = self.chat([
            {"role": "system", "content": sys},
            {"role": "user", "content": user},
        ])
        if not msg:
            return None
        msg = msg.replace("`", "").replace("*", "").replace("_", "").replace("[", "").replace("]", "")
        return msg.strip()
