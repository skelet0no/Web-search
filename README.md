# Sitemap crawler

Поисковый робот на Python для Игромании, StopGame и VGTimes. Адреса получает только из sitemap, сохраняет исходный HTML в MongoDB и продолжает работу после остановки.

Токенизация извлечённых текстов реализована на C++ с прямым чтением MongoDB.
Лемматизация русских слов также реализована на C++17 (XMorphy + ICU), отдельным
этапом после токенизации

## Тесты токенизатора на C++

Оба набора в `tokenizer/tests` написаны на C++ и запускаются через CTest:

```bash
cmake -S . -B build-local -DLEMMATIZER_BUILD=OFF -DTOKENIZER_WITH_MONGO=ON -DBUILD_TESTING=ON
cmake --build build-local --target tokenizer_tests tokenizer_repository_tests --parallel 4
docker compose start mongo
TOKENIZER_TEST_URI='mongodb://127.0.0.1:27017/?serverSelectionTimeoutMS=3000' \
  ctest --test-dir build-local -R '^tokenizer_' --output-on-failure
```

`tokenizer_core` проверяет выделение и нормализацию токенов, Unicode, UTF-8,
байтовые границы и статистику без подключения к БД. `tokenizer_repository`
запускает собранное приложение и проверяет сохранение токенов и отчётов,
фильтры, лимиты, выбор корпуса, ошибки документов, разбиение на части,
повторные идентификаторы и ошибки записи. Для каждого сценария создаётся
отдельная база `tokenizer_test_<id>` и временный каталог; после выполнения
они удаляются. Рабочая база `crawler` не изменяется.

Без `TOKENIZER_TEST_URI` интеграционный набор отмечается как пропущенный.
Путь к бинарнику CTest передаёт автоматически; Python и `TOKENIZER_BINARY`
для этих тестов не нужны. Интеграционный набор использует POSIX `fork`/`exec`
и предназначен для Linux/macOS.

## Анализ корпуса для лабораторных работ

Пакет `corpus` читает документы MongoDB, выделяет текст и сохраняет статистику,
характеристики HTML и примеры в `docx/runs/<run_id>/`.

```bash
python -m corpus inspect config.yaml --uri mongodb://127.0.0.1:27017 --source vgtimes --limit 30
```

Диагностика только читает базу и доступна при работающем crawler. Полный проход
`extract` выполняется при остановленном crawler и сохраняет текст в отдельной
коллекции.

## Запуск через Docker Compose

```bash
cp config.example.yaml config.yaml
# При необходимости измените config.yaml.
docker compose up --build -d
docker compose logs -f crawler
```

MongoDB хранит данные в постоянном томе `mongo_data`. База доступна локально на `127.0.0.1:27017`. Остановка и продолжение:

```bash
docker compose stop crawler
docker compose start crawler
```

После изменения YAML используйте `docker compose restart crawler`. После изменения кода — `docker compose up --build -d crawler`. Не удаляйте том, если требуется сохранить документы и очередь. В базе допускается один экземпляр робота: блокировка с автоматически продлеваемым сроком предотвращает одновременный запуск двух процессов.

Если при сборке Docker не работает DNS, исправьте DNS Docker либо соберите образ с сетью хоста на Linux:

```bash
docker build --network host -t web-search-crawler .
docker compose up --no-build -d
```

## Запуск без контейнера робота

Python 3.11 или новее:

```bash
python -m venv .venv
source .venv/bin/activate
pip install -e '.[test]'
docker compose up -d mongo
```

В локальном `config.yaml` замените `db.uri` на `mongodb://127.0.0.1:27017`, затем:

```bash
python -m crawler config.yaml
```

Это единственный позиционный аргумент. `Ctrl+C` или SIGTERM прекращают выдачу новых задач. При аварии захваченные задачи доступны после истечения `lease_seconds` (по умолчанию 300 секунд). Сохранённые документы не дублируются; HTTP-запрос, прерванный аварией, может повториться. Незавершённая sitemap читается заново с безопасным повторным добавлением URL.

## Настройки и фазы

В `db` задаются URI и название базы. В `sources` — имя, домен, стартовые sitemap, задержка между началами запросов и параллелизм. Адреса карт также читаются из robots.txt. Вложенные sitemap и gzip поддерживаются; HTML-ссылки не обходятся. Карты и документы ограничены точным доменом источника, включая перенаправления.

Основные параметры `logic`:

| Параметр | Назначение |
| --- | --- |
| `target_documents` | Общая цель: 1000 для проверки, 1000000 для полного сбора |
| `recrawl_interval_days` | Интервал успешных повторных проверок, в днях |
| `request_timeout_seconds` | Таймаут сетевой операции |
| `max_retries` | Максимальное число попыток первичной задачи |
| `retry_base_seconds` | Начальная задержка экспоненциального повтора |
| `lease_seconds` | Срок захвата задачи и блокировки процесса |
| `max_html_bytes` | Максимальный размер HTML, до 8000000 байт |
| `max_sitemap_bytes` | Лимит скачанной и распакованной карты |
| `poll_seconds` | Пауза при отсутствии готовой работы |
| `user_agent` | Идентификатор робота |

Сначала робот собирает уникальные HTML-документы с HTTP 200. Начальные квоты примерно равны: 334/333/333. При исчерпании sitemap источника остаток передаётся другим. Ошибки не считаются исчерпанием. Достижение цели включает фазу повторной проверки; до этого старые страницы не перепроверяются. Увеличение цели и перезапуск возобновляют первичный сбор.

После успешной проверки следующая назначается через N дней. При наличии ETag/Last-Modified используется условный GET. При 304 HTML сохраняется, при 200 сравнивается SHA-256 исходных байтов. При изменении заменяется последняя версия; история HTML не ведётся. 404/410 отмечаются без удаления последней сохранённой версии. Ошибки повторной проверки откладываются с экспоненциальной задержкой до примерно 8,5 часа при стандартных настройках и повторяются дальше.

Задержки действуют и при повторной обкачке. Robots.txt обновляется раз в сутки; учитываются запреты, wildcard-правила, Crawl-delay и Request-rate. При 429/5xx учитывается Retry-After. Если проверять все документы за N дней невозможно, очередь отстаёт, а ограничения остаются в силе.

## Данные и диагностика

- `documents`: URL, HTML, источник, Unix timestamp `crawled_at`, хеш, валидаторы, последняя и следующая проверки, HTTP-статус.
- `crawl_queue`: постоянные задачи sitemap/страниц, попытки, ошибки и захваты.
- `state`: блокировка процесса, фаза и счётчики.

Прогресс выводится примерно раз в 30 секунд. При недостатке адресов или исчерпании попыток робот завершает работу с кодом 2, сохраняя данные. При достижении цели продолжает работать как планировщик повторных проверок.

Просмотр состояния:

```bash
docker compose exec mongo mongosh crawler --quiet --eval 'db.state.findOne({_id:"progress"})'
docker compose exec mongo mongosh crawler --quiet --eval 'db.documents.countDocuments({})'
docker compose exec mongo mongosh crawler --quiet --eval 'db.crawl_queue.find({status:"failed"},{url:1,last_error:1}).limit(10).toArray()'
```

После устранения причины ошибок остановите робот, верните неудачные задачи в очередь и запустите снова:

```bash
docker compose stop crawler
docker compose exec mongo mongosh crawler --quiet --eval 'db.crawl_queue.updateMany({status:"failed"},{$set:{status:"pending",attempts:0,next_attempt:0}})'
docker compose start crawler
```
