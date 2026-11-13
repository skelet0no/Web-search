import gzip
import io
from xml.etree.ElementTree import ParseError

from defusedxml.common import DefusedXmlException

from crawler.robots import RobotsRules
from crawler.sitemaps import entries
from crawler.urls import normalize
from tests.support import UnitTestCase


class UrlTests(UnitTestCase):
    def test_normalization_preserves_content_parameters(self):
        self.assertEqual(normalize(" HTTPS://EXAMPLE.TEST:443/a?utm_source=x&ID=2&empty=&fbclid=y#part ",
                                   "example.test"), "https://example.test/a?ID=2&empty=")
        self.assertEqual(normalize("https://example.test", "example.test"), "https://example.test/")
        self.assertEqual(normalize("http://example.test:80/a?a=1&a=2", "example.test"),
                         "http://example.test/a?a=1&a=2")

    def test_domain_scheme_credentials_and_port_boundaries(self):
        for url in ("//example.test/x", "ftp://example.test/x", "https://sub.example.test/x",
                    "https://example.test.evil/x", "https://user:pass@example.test/x",
                    "https://example.test:8080/x", "https://example.test:invalid/x"):
            with self.subTest(url=url), self.assertRaises(ValueError):
                normalize(url, "example.test")


class SitemapTests(UnitTestCase):
    def test_namespace_plain_and_gzip(self):
        xml = (b'<urlset xmlns="http://www.sitemaps.org/schemas/sitemap/0.9">'
               b'<url><loc> https://example.test/a </loc><lastmod>2026-01-02</lastmod></url>'
               b'<url><lastmod>ignored</lastmod></url></urlset>')
        for content in (xml, gzip.compress(xml)):
            with self.subTest(compressed=content != xml):
                self.assertEqual(list(entries(io.BytesIO(content), 10000)),
                                 [("page", "https://example.test/a", "2026-01-02")])

    def test_nested_sitemap_type(self):
        xml = b'<sitemapindex><sitemap><loc>https://example.test/sub.xml.gz</loc></sitemap></sitemapindex>'
        self.assertEqual(list(entries(io.BytesIO(xml), 10000)),
                         [("sitemap", "https://example.test/sub.xml.gz", None)])

    def test_uncompressed_budget_including_gzip_bomb(self):
        xml = b'<urlset>' + b' ' * 10000 + b'</urlset>'
        for content in (xml, gzip.compress(xml)):
            with self.subTest(compressed=content != xml), self.assertRaises(ValueError):
                list(entries(io.BytesIO(content), 100))

    def test_bad_xml_and_entity_expansion(self):
        for content in (b'<html/>', b'<urlset><url>',
                        b'<!DOCTYPE urlset [<!ENTITY x "boom">]><urlset><url><loc>&x;</loc></url></urlset>'):
            with self.subTest(content=content), self.assertRaises((ValueError, ParseError, DefusedXmlException)):
                list(entries(io.BytesIO(content), 10000))


class RobotsTests(UnitTestCase):
    def rules(self, text):
        rules = RobotsRules("https://example.test/robots.txt")
        rules.parse(text.splitlines())
        return rules

    def test_longest_rule_and_allow_tie(self):
        rules = self.rules("User-agent: *\nDisallow: /private\nAllow: /private/open\nDisallow: /equal\nAllow: /equal")
        for path, allowed in (("/", True), ("/private/a", False), ("/private/open/a", True), ("/equal", True)):
            with self.subTest(path=path):
                self.assertEqual(rules.can_fetch("UnitBot", "https://example.test" + path), allowed)

    def test_specific_agent_overrides_wildcard_and_merges_groups(self):
        rules = self.rules("User-agent: *\nDisallow: /\n\nUser-agent: UnitBot\nDisallow: /a\n"
                           "\nUser-agent: UnitBot\nDisallow: /b")
        self.assertTrue(rules.can_fetch("UnitBot/1.0", "https://example.test/c"))
        self.assertFalse(rules.can_fetch("UnitBot/1.0", "https://example.test/b"))
        self.assertFalse(rules.can_fetch("Other", "https://example.test/c"))

    def test_wildcard_end_anchor_query_and_comments(self):
        rules = self.rules("User-agent: *\nDisallow: /*.pdf$ # documents\nDisallow: /*?secret=*")
        self.assertFalse(rules.can_fetch("bot", "https://example.test/a.pdf"))
        self.assertTrue(rules.can_fetch("bot", "https://example.test/a.pdf/more"))
        self.assertFalse(rules.can_fetch("bot", "https://example.test/a?secret=1"))

    def test_delays_rates_sitemaps_and_empty_rules(self):
        rules = self.rules("User-agent: *\nCrawl-delay: 3\nRequest-rate: 2/10\nSitemap: https://example.test/map.xml")
        self.assertEqual(rules.crawl_delay("bot"), 3)
        self.assertEqual(tuple(rules.request_rate("bot")), (2, 10))
        self.assertEqual(rules.site_maps(), ["https://example.test/map.xml"])
        self.assertTrue(self.rules("").can_fetch("bot", "https://example.test/"))
