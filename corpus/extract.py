import json
import re
from importlib.metadata import version

from bs4 import BeautifulSoup, Comment, NavigableString, Tag

EXTRACTOR_VERSION = "corpus-2.0-bs4-" + version("beautifulsoup4") + "-html.parser"
SELECTORS = {
    "vgtimes": (".news_item .text_block", ".tag_description", ".module_tags"),
    "igromania": ("[class*='material-content_']", "[class*='TextContent_text']",
                  "[class*='HeadingPage_infiniteWrap']"),
    "stopgame": ("#material_content", "[class*='_game-info_']", ".article-body", ".article__body", ".article_text"),
}
COLLECTION_SELECTORS = {".tag_description", ".module_tags", "[class*='HeadingPage_infiniteWrap']"}
NOISE_SELECTORS = {
    "vgtimes": ".banner_in_news, .tg_ad, .relrel, .news_content_bottom, .author_block",
    "igromania": "[class*='AdFoxBanner'], [class*='knb-banner'], [class*='PlotWithMaterials'], "
                 "[class*='CommentsOfArticle'], [class*='CommentsFormOfArticle'], [class*='knb-footer']",
    "stopgame": "[class*='_comment_'], [class*='_comments_'], [class*='_read-more_']",
}
AUTHOR_SELECTORS = {
    "vgtimes": ".news_item_top a.author",
    "igromania": "[class*='HeaderOfArticle_head'] [class*='AuthorOfArticle_author']",
    "stopgame": "[data-mainmaterial-author]",
}
BLOCKS = {"p", "div", "section", "article", "main", "blockquote", "ul", "ol",
          "li", "h1", "h2", "h3", "h4", "h5", "h6", "figure", "figcaption", "table", "tr", "dl", "dt", "dd"}
NOISE = re.compile(r"(?:^|[-_\s])(?:comments?|replies|sidebar|advert(?:isement)?|ads|"
                   r"related|recommendations?|social|share|sharing|breadcrumbs?|"
                   r"newsletter|subscribe|subscription|promo)(?:$|[-_\s])", re.I)
FEATURE_TAGS = ("article", "main", "h1", "p", "li", "table", "pre", "code", "img")


def normalize(value):
    return re.sub(r"\s+", " ", value).strip()


def article_metadata(soup):
    for script in soup.find_all("script", type="application/ld+json"):
        try:
            data = json.loads(script.string or script.get_text())
        except (ValueError, TypeError):
            continue
        nodes = list(data) if isinstance(data, list) else [data]
        while nodes:
            node = nodes.pop(0)
            if not isinstance(node, dict):
                continue
            kind = node.get("@type", [])
            kinds = [kind] if isinstance(kind, str) else kind if isinstance(kind, list) else []
            if any(k in {"Article", "NewsArticle", "BlogPosting", "Review"} for k in kinds if isinstance(k, str)):
                return node
            graph = node.get("@graph")
            if isinstance(graph, list):
                nodes.extend(graph)
    return {}


def metadata_text(value):
    if isinstance(value, dict):
        value = value.get("name")
    if isinstance(value, str):
        return normalize(value) or None
    if isinstance(value, list):
        return ", ".join(name for item in value if (name := metadata_text(item))) or None
    return None


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
                  extraction_method=None, page_type="unknown", warnings=[], features={})
    if not isinstance(raw_html, str) or not raw_html.strip():
        result["warnings"] = ["missing_or_empty_html"]
        return result
    result["raw_utf8_bytes"] = len(raw_html.encode("utf-8"))
    soup = BeautifulSoup(raw_html, "html.parser")
    result["features"] = {tag: bool(soup.find(tag)) for tag in FEATURE_TAGS}
    result["features"]["json_ld"] = bool(soup.find("script", type="application/ld+json"))
    result["features"]["open_graph"] = bool(soup.find("meta", property=re.compile(r"^og:")))
    structured = article_metadata(soup)

    def meta(*names):
        for name in names:
            tag = soup.find("meta", attrs={"property": name}) or soup.find("meta", attrs={"name": name})
            if tag and isinstance(tag.get("content"), str) and tag["content"].strip():
                return normalize(tag["content"])
        return None

    heading = soup.find("h1")
    result["title"] = (normalize(heading.get_text(" ")) if heading else None) or metadata_text(structured.get("headline")) or meta("og:title") or (
        normalize(soup.title.get_text(" ")) if soup.title else None)
    result["language"] = (soup.html.get("lang") if soup.html else None) or metadata_text(structured.get("inLanguage")) or meta("og:locale")
    result["author"] = meta("author") or metadata_text(structured.get("author")) or meta("article:author")
    if not result["author"] and source in AUTHOR_SELECTORS:
        author = soup.select_one(AUTHOR_SELECTORS[source])
        if author:
            result["author"] = normalize(author.get_text(" ")) or None
    result["published_at"] = meta("article:published_time", "datePublished", "pubdate") or metadata_text(structured.get("datePublished"))
    if not result["published_at"]:
        time_tag = soup.select_one("time[itemprop~='datePublished'][datetime], meta[itemprop~='datePublished'][content]")
        if time_tag:
            result["published_at"] = time_tag.get("datetime") or time_tag.get("content")

    for node in list(soup.find_all(["script", "style", "noscript", "template", "nav", "aside", "footer", "form", "iframe", "button", "svg"])):
        node.decompose()
    if source in NOISE_SELECTORS:
        for node in soup.select(NOISE_SELECTORS[source]):
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
            root = candidates[0] if selector in SELECTORS.get(source, ()) else max(candidates, key=lambda tag: len(tag.get_text()))
            result["extraction_method"] = selector
            result["page_type"] = "collection" if selector in COLLECTION_SELECTORS else (
                "game" if selector == "[class*='_game-info_']" else "article")
            if len(candidates) > 1:
                result["warnings"].append("multiple_content_blocks_requires_review")
            break
    if root is None:
        root = soup.body or soup
        result["extraction_method"] = "visible_text_fallback"
        result["warnings"].append("fallback_requires_review")
    if result["page_type"] == "collection":
        result["warnings"].append("collection_page")
    elif result["page_type"] == "game":
        result["warnings"].append("game_card")
        if root.find("p") is None:
            result["warnings"].append("game_card_without_description")
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
