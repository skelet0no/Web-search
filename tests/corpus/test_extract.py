from bs4 import BeautifulSoup

from corpus.extract import article_metadata, extract_document, metadata_text, normalize, render_text
from tests.support import UnitTestCase


class ExtractionTests(UnitTestCase):
    def test_non_html_and_empty_input(self):
        for raw in (None, 7, "", " \n"):
            with self.subTest(raw=raw):
                result = extract_document(raw, "unknown")
                self.assertEqual(result["status"], "unsupported")
                self.assertIn("missing_or_empty_html", result["warnings"])

    def test_source_specific_selectors_and_noise_removal(self):
        cases = (("stopgame", '<div id="material_content">{}</div>'),
                 ("vgtimes", '<div class="news_item"><div class="text_block">{}</div></div>'),
                 ("igromania", '<div class="material-content_main">{}</div>'))
        for source, template in cases:
            with self.subTest(source=source):
                html = '<html lang="ru"><body><nav>navigation</nav><h1>Игры</h1>' + template.format(
                    '<p>Полезный <b>текст</b>.</p><script>script</script><div class="comments">noise</div>') + '</body></html>'
                result = extract_document(html, source)
                self.assertEqual(result["status"], "ok")
                self.assertEqual(result["title"], "Игры")
                self.assertIn("Полезный текст", result["text"])
                for noise in ("navigation", "script", "noise"):
                    self.assertNotIn(noise, result["text"])
                self.assertEqual(result["raw_utf8_bytes"], len(html.encode("utf-8")))
                self.assertEqual(result["text_utf8_bytes"], len(result["text"].encode("utf-8")))

    def test_structured_graph_and_metadata_priority(self):
        html = '''<html lang="ru-RU"><head><title>Fallback</title>
        <meta property="og:title" content="OG"><meta name="author" content="Meta Author">
        <script type="application/ld+json">{"@graph":[{"@type":["Thing","NewsArticle"],
        "headline":"JSON headline", "author":{"name":"JSON author"},
        "datePublished":"2026-01-01", "inLanguage":"en"}]}</script></head>
        <body><h1> Main   title </h1><article><p>Body</p></article></body></html>'''
        result = extract_document(html, "unknown")
        self.assertEqual(result["title"], "Main title")
        self.assertEqual(result["author"], "Meta Author")
        self.assertEqual(result["language"], "ru-RU")
        self.assertEqual(result["published_at"], "2026-01-01")
        self.assertTrue(result["features"]["json_ld"])
        self.assertTrue(result["features"]["open_graph"])

    def test_title_fallback_chain(self):
        for head, expected in (
            ('<script type="application/ld+json">{"@type":"Article","headline":"JSON"}</script>', "JSON"),
            ('<meta property="og:title" content="Open Graph">', "Open Graph"),
            ('<title>Page title</title>', "Page title"), ("", None),
        ):
            with self.subTest(expected=expected):
                self.assertEqual(extract_document(f"<head>{head}</head><body><article>Body</article></body>", "x")["title"], expected)

    def test_heading_not_duplicated_and_hidden_content_removed(self):
        result = extract_document('<article><h1>Title</h1><p>Body</p><p hidden>hidden</p>'
                                  '<p aria-hidden="true">aria</p><aside>aside</aside></article>', "x")
        self.assertEqual(result["text"], "Body")

    def test_largest_generic_article_and_ambiguous_blocks_warning(self):
        result = extract_document('<article>small</article><article>A much longer body</article>', "x")
        self.assertEqual(result["text"], "A much longer body")
        self.assertIn("multiple_content_blocks_requires_review", result["warnings"])

    def test_fallback_collection_game_and_empty_warnings(self):
        for html, source, warning, kind in (
            ("<body>Text</body>", "x", "fallback_requires_review", "unknown"),
            ('<div class="tag_description">Games</div>', "vgtimes", "collection_page", "collection"),
            ('<div class="_game-info_main">Game</div>', "stopgame", "game_card_without_description", "game"),
        ):
            with self.subTest(kind=kind):
                result = extract_document(html, source)
                self.assertEqual(result["page_type"], kind)
                self.assertIn(warning, result["warnings"])
        result = extract_document("<article><script>no text</script></article>", "x")
        self.assertEqual(result["status"], "empty")
        self.assertIn("no_text_in_selected_block", result["warnings"])

    def test_preformatted_tables_linebreaks_entities_and_replacement_warning(self):
        html = '<article><p>A&nbsp;B<br>C</p><!-- ignore --><pre>  x\n    y</pre><table><tr><td>One</td><td>Two</td></tr></table><p>�</p></article>'
        result = extract_document(html, "x")
        self.assertIn("A B\nC", result["text"])
        self.assertIn("  x\n    y", result["text"])
        self.assertIn("One\tTwo", result["text"])
        self.assertNotIn("ignore", result["text"])
        self.assertIn("replacement_characters_in_saved_html", result["warnings"])

    def test_metadata_helpers_handle_malformed_and_nested_values(self):
        soup = BeautifulSoup('<script type="application/ld+json">bad</script>'
                             '<script type="application/ld+json">[0,{"@type":"Review","headline":"ok"}]</script>', "html.parser")
        self.assertEqual(article_metadata(soup)["headline"], "ok")
        self.assertEqual(metadata_text([{"name": " A "}, "B", 3]), "A, B")
        self.assertIsNone(metadata_text({"other": "ignored"}))
        self.assertEqual(normalize(" A\n B\t C "), "A B C")
        self.assertEqual(render_text(BeautifulSoup("<p>A</p><p>B</p>", "html.parser")), "A\n\nB")
