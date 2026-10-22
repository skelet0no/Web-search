import csv
import hashlib
import json
from pathlib import Path


def cell(value):
    if value is None:
        return "—"
    if isinstance(value, float):
        return f"{value:.2f}"
    return str(value).replace("|", "\\|").replace("\n", " ").replace("\r", " ")


def write_report(directory, summary):
    directory = Path(directory)
    stats = summary["statistics"]
    lines = ["# Анализ корпуса MongoDB", "", f"Запуск: `{summary['run_id']}`.", "",
             f"Состояние: **{summary['state']}**. Режим: **{'выборка' if summary['is_sample'] else 'полный проход выбранного набора'}**.",
             "",
             f"Начало: {summary['started_at']}. Завершение: {summary.get('finished_at', '—')}.", "",
             ("Диагностическая выборка из работающей базы без общего снимка. "
              "Записи MongoDB не изменялись."
              if summary.get("snapshot_mode") == "live_preview" else
              "Состав зафиксирован блокировкой crawler на время обработки. Исходные документы не изменялись."), "",
             "## Размеры и количество", "",
             "Размеры — байты UTF-8 сохранённого HTML и выделенного текста. Сетевые байты и размер BSON не измеряются.", "",
             "Средние считаются по всем выбранным документам; при ошибке извлечения объём текста равен нулю.", "",
             "| Набор | Документов | HTML, байт | Текст, байт | Средний HTML | Средний текст | Средний текст успешных |",
             "| --- | --- | --- | --- | --- | --- | --- |"]
    metrics = ("count", "raw_utf8_bytes", "text_utf8_bytes", "average_raw_utf8_bytes",
               "average_text_utf8_bytes", "average_successful_text_utf8_bytes")
    for name, totals in [("Всего", stats["total"]), *stats["sources"].items()]:
        lines.append("| " + " | ".join([cell(name), *(cell(totals[k]) for k in metrics)]) + " |")
    lines += ["", "## HTML-разметка и метаданные", "",
              "Числа обозначают количество документов с соответствующим признаком, не количество тегов.", "",
              "| Набор | Признаки HTML | Наличие метаданных |", "| --- | --- | --- |"]
    for source, totals in stats["sources"].items():
        lines.append("| " + " | ".join(map(cell, (source, json.dumps(totals["features"], ensure_ascii=False),
                                                    json.dumps(totals["metadata_present"], ensure_ascii=False)))) + " |")
    lines += ["", "## Примеры", "",
              "По каждому источнику сохранены самый маленький, самый большой и дополнительные документы, выбранные по хешу ID.",
              "Примеры служат для изучения и не являются репрезентативной оценкой качества.", "",
              "| Источник | Тип страницы | URL | HTML, байт | Текст, байт | HTML / TXT |", "| --- | --- | --- | --- | --- | --- |"]
    for sample in summary.get("samples", []):
        lines.append(f"| {cell(sample['source'])} | {cell(sample['page_type'])} | {cell(sample['url'])} | {sample['raw_utf8_bytes']} | "
                     f"{sample['text_utf8_bytes']} | [HTML]({sample['html_path']}) / [TXT]({sample['text_path']}) |")
    lines.append("")
    (directory / "report.md").write_text("\n".join(lines), encoding="utf-8")


class Samples:
    def __init__(self, per_source):
        self.limit = per_source
        self.groups = {}

    def add(self, original, record):
        source = record["source"]
        group = self.groups.setdefault(source, {})
        key = str(record["document_id"])
        group[key] = (original["raw_html"], record)
        values = list(group)
        smallest = min(values, key=lambda k: (group[k][1]["raw_utf8_bytes"], k))
        largest = max(values, key=lambda k: (group[k][1]["raw_utf8_bytes"], k))
        selected = {smallest, largest}
        others = sorted((k for k in values if k not in selected), key=lambda k: hashlib.sha256(k.encode()).hexdigest())
        selected.update(others[:self.limit - len(selected)])
        self.groups[source] = {k: group[k] for k in selected}

    def export(self, directory):
        directory = Path(directory)
        samples_dir = directory / "samples"
        samples_dir.mkdir()
        rows = []
        for source in sorted(self.groups):
            for key, (html, record) in sorted(self.groups[source].items()):
                basename = hashlib.sha256((source + ":" + key).encode()).hexdigest()
                html_path, text_path = f"samples/{basename}.html", f"samples/{basename}.txt"
                (directory / html_path).write_text(html, encoding="utf-8")
                (directory / text_path).write_text(record["text"], encoding="utf-8")
                rows.append(dict(document_id=key, source=source, url=record["url"],
                                 raw_utf8_bytes=record["raw_utf8_bytes"], text_utf8_bytes=record["text_utf8_bytes"],
                                 status=record["status"], page_type=record["page_type"], html_path=html_path, text_path=text_path))
        with (directory / "samples.csv").open("w", encoding="utf-8", newline="") as stream:
            writer = csv.DictWriter(stream, fieldnames=["document_id", "source", "url", "raw_utf8_bytes",
                                                       "text_utf8_bytes", "status", "page_type", "html_path", "text_path"])
            writer.writeheader()
            writer.writerows(rows)
        return rows
