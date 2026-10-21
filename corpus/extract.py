import re
from importlib.metadata import version

from bs4 import BeautifulSoup, Comment, NavigableString, Tag

EXTRACTOR_VERSION = "corpus-1.0-bs4-" + version("beautifulsoup4") + "-html.parser"
SELECTORS = {
    "habr": (".tm-article-body", ".article-formatted-body", "#post-content-body"),
    "aif": (".article_text", ".article-text"),
    "stopgame": (".article-body", ".article__body", ".article_text"),
}
BLOCKS = {"p", "div", "section", "article", "main", "blockquote", "ul", "ol",
          "li", "h1", "h2", "h3", "h4", "h5", "h6", "figure", "figcaption", "table", "tr"}
NOISE = re.compile(r"(?:^|[-_\s])(?:comments?|replies|sidebar|advert(?:isement)?|ads|"
                   r"related|recommendations?|social|share|sharing|breadcrumbs?|"
                   r"newsletter|subscribe|subscription|promo)(?:$|[-_\s])", re.I)
FEATURE_TAGS = ("article", "main", "h1", "p", "li", "table", "pre", "code", "img")


def normalize(value):
    return re.sub(r"\s+", " ", value).strip()


def render_text(root):
    parts = []
    stack = [(root, False)]
    while stack:
        node, closing = stack.pop()
        if isinstance(node, Comment):
            continue
        if isinstance(node, NavigableString):
            parts.append(re.sub(r"\s+", " ", str(node)))
            continue
        if not isinstance(node, Tag):
            continue
        if closing:
            if node.name in BLOCKS:
                parts.append("\n\n")
            elif node.name in {"td", "th"}:
                parts.append("\t")
            continue
        if node.name == "pre":
            parts.extend(("\n\n", node.get_text().replace("\r\n", "\n").strip("\n"), "\n\n"))
            continue
        if node.name == "br":
            parts.append("\n")
            continue
        if node.name in BLOCKS:
            parts.append("\n\n")
        stack.append((node, True))
        stack.extend((child, False) for child in reversed(list(node.children)))
    text = "".join(parts).replace("\xa0", " ")
    text = re.sub(r"[ \t]+\n", "\n", text)
    text = re.sub(r"\n{3,}", "\n\n", text)
    return text.strip()


def extract_document(raw_html, source):
    result = dict(title=None, text="", language=None, author=None, published_at=None,
                  raw_utf8_bytes=0, text_utf8_bytes=0, status="unsupported",
                  extraction_method=None, warnings=[], features={})
    if not isinstance(raw_html, str) or not raw_html.strip():
        result["warnings"] = ["missing_or_empty_html"]
        return result
    result["raw_utf8_bytes"] = len(raw_html.encode("utf-8"))
    soup = BeautifulSoup(raw_html, "html.parser")
    result["features"] = {tag: bool(soup.find(tag)) for tag in FEATURE_TAGS}
    result["features"]["json_ld"] = bool(soup.find("script", type="application/ld+json"))
    result["features"]["open_graph"] = bool(soup.find("meta", property=re.compile(r"^og:")))

    def meta(*names):
        for name in names:
            tag = soup.find("meta", attrs={"property": name}) or soup.find("meta", attrs={"name": name})
            if tag and isinstance(tag.get("content"), str) and tag["content"].strip():
                return normalize(tag["content"])
        return None

    heading = soup.find("h1")
    result["title"] = (normalize(heading.get_text(" ")) if heading else None) or meta("og:title") or (
        normalize(soup.title.get_text(" ")) if soup.title else None)
    result["language"] = (soup.html.get("lang") if soup.html else None) or meta("og:locale")
    result["author"] = meta("author", "article:author")
    result["published_at"] = meta("article:published_time", "datePublished", "pubdate")
    if not result["published_at"]:
        time_tag = soup.find("time", attrs={"datetime": True})
        if time_tag and time_tag.get("itemprop") == "datePublished":
            result["published_at"] = time_tag["datetime"]

    for node in list(soup.find_all(["script", "style", "noscript", "template", "nav", "aside", "footer", "form", "iframe"])):
        node.decompose()
    for node in list(soup.find_all(True)):
        if node.attrs is None:
            continue
        marker = " ".join([str(node.get("id", "")), " ".join(node.get("class", []))])
        if node.has_attr("hidden") or node.get("aria-hidden") == "true" or NOISE.search(marker):
            node.decompose()

    root = None
    for selector in (*SELECTORS.get(source, ()), "[itemprop='articleBody']", "article", "main"):
        candidates = soup.select(selector)
        if candidates:
            root = max(candidates, key=lambda tag: len(tag.get_text()))
            result["extraction_method"] = selector
            if len(candidates) > 1:
                result["warnings"].append("multiple_content_blocks_requires_review")
            break
    if root is None:
        root = soup.body or soup
        result["extraction_method"] = "visible_text_fallback"
        result["warnings"].append("fallback_requires_review")
    for tag in list(root.find_all("h1")):
        if normalize(tag.get_text(" ")) == result["title"]:
            tag.decompose()
    for tag in list(root.find_all(["head", "title"])):
        tag.decompose()
    result["text"] = render_text(root)
    result["text_utf8_bytes"] = len(result["text"].encode("utf-8"))
    result["status"] = "ok" if result["text"] else "empty"
    if not result["text"]:
        result["warnings"].append("no_text_in_selected_block")
    if "\ufffd" in raw_html:
        result["warnings"].append("replacement_characters_in_saved_html")
    return result
