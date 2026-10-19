from urllib.parse import parse_qsl, urlencode, urlsplit, urlunsplit

TRACKING = {"fbclid", "gclid", "yclid", "utm", "__cf_chl_tk"}


def normalize(url: str, domain: str) -> str:
    parts = urlsplit(url.strip())
    if parts.scheme.lower() not in ("http", "https") or parts.hostname != domain:
        raise ValueError("URL outside allowed domain")
    if parts.username or parts.password or parts.port not in (None, 80, 443):
        raise ValueError("Unexpected credentials or port")
    query = [(k, v) for k, v in parse_qsl(parts.query, keep_blank_values=True)
             if not k.lower().startswith("utm_") and k.lower() not in TRACKING]
    return urlunsplit((parts.scheme.lower(), domain, parts.path or "/", urlencode(query), ""))
