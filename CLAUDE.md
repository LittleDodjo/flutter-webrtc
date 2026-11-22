# flutter-webrtc — форк плагина для Squeak

Общие правила — в `~/.claude/CLAUDE.md`. Здесь только своё для этого репо.
Карточки **SQC**. Апстрим — `upstream` (flutter-webrtc/flutter-webrtc), наш — `origin` (LittleDodjo).

## Цепочка

```
webrtc (ядро, m144_release webrtc-sdk)  ← не патчим
  → libwebrtc ветка squeak-nvenc: NVENC/AMF/MF, GPU-захват, APM → тег → релиз с libwebrtc.dll/.so
    → flutter-webrtc ветка squeak-nvenc-render: third_party/libwebrtc_version.ini + C++ плагина
      → Squeak: dependency_overrides в pubspec.yaml (git, ref squeak-nvenc-render), pubspec.lock держит коммит
```

## Ветки

- **`squeak-nvenc-render` — единственная живая**, её тянет клиент. Работать только в ней.
- `squeak`, `squeak-nvenc`, `squeak-render` — старые ступени (сентябрь), полностью влиты в
  `squeak-nvenc-render`. Не коммитить в них, не удалять без просьбы.
- `main` — зеркало апстрима.

## Версии — поднимать только вместе

- Плагин 1.6.0 на WebRTC **m144** (`binary_version = libwebrtc.m144.7559.09-nvenc.N`).
- Клиент держит `livekit_client <2.13`: 2.13+ требует WebRTC 150 и flutter_webrtc ≥ 1.6.2.
  Переход на m150 = libwebrtc + этот форк + livekit_client в клиенте одной карточкой.
- Мерж апстрима — только по карточке: он тянет свой бинарь и ломает пин.

## Новый движок libwebrtc

1. В libwebrtc вышел релиз `libwebrtc.m144.7559.09-nvenc.N`.
2. Здесь в `third_party/libwebrtc_version.ini`: `binary_version` и `sha256_win_x64`/`sha256_linux_x64`
   из `*.zip.shasum` релиза (без хеша своей ОС CMake падает — так задумано), и абзац в шапке ini:
   что изменилось с `-nvenc.N` и где проверено.
3. Коммит `chore(libwebrtc): движок nvenc.N — … (SQC-N)`, push.
4. В клиенте — `flutter pub upgrade flutter_webrtc` (обновит `resolved-ref` в lock), коммит по правилам клиента.

Заголовки `third_party/libwebrtc/include` приходят из архива; API менять в libwebrtc, а не здесь.

## Своё в форке

- C++ десктопа: `common/cpp` (общий для windows/linux), `windows/`, `linux/`. Рендер плитки — через
  D3D11-текстуру (`GpuSurfaceTexture`), не копированием кадра.
- Шумодав захвата: `third_party/squeak_df` (DeepFilterNet3, Rust), запасной — `third_party/rnnoise`.
  Бинарь DF собирает `squeak_df.yml` по тегу `squeak_df-vN`, пин — секция `[squeak_df]` в том же ini.
- Тест врезки на настоящем APM: `common/cpp/test/squeak_apm_test.cc`, CI `apm-test.yml`.

## CI

- `build.yml` (апстримный: format/analyze, сборки платформ) и `apm-test.yml` — на push/PR в
  `squeak-nvenc-render`.
- `publish.yaml` — тег `v*` публикует в pub.dev. **Теги `v*` не ставить никогда.**
- Тег `squeak_df-vN` — только когда меняется сам DeepFilterNet3.

## Проверка

- `dart format` и `flutter analyze` по Dart-части; C++ — `clang-format` (`.clang-format`).
- Нативное поведение (захват, NVENC, звук) проверяется только на Windows-устройстве через клиент —
  в комментарии карточки писать, что проверено на устройстве, а что нет.

## Коммиты

Как в истории ветки: `fix(cpp): …`, `feat(windows,linux): …`, `chore(libwebrtc): …` — описание по-русски,
в конце `(SQC-N)`. Апстримные коммиты и их сообщения не переписывать.
